"""D11.12 paired volume exchange. Geometry and proposals remain on CPU."""
import warp as wp

@wp.kernel
def artistic_paired_exchange(
    endpoints: wp.array(dtype=wp.int32), amount: wp.array(dtype=float),
    caps: wp.array(dtype=float), accum: wp.array(dtype=float),
    count: int, phase: int,
):
    i = wp.tid()
    if phase == 0:
        accum[i] = 0.0
    else:
        a = endpoints[2*i]
        b = endpoints[2*i+1]
        f = amount[i]
        if f > 0.0:
            if phase == 1:
                wp.atomic_add(accum,a,f)
                wp.atomic_add(accum,count+b,f)
            else:
                scale = wp.min(1.0,wp.min(caps[a]/accum[a],caps[count+b]/accum[count+b]))
                mass = f*scale
                wp.atomic_add(accum,2*count+a,-mass)
                wp.atomic_add(accum,2*count+b,mass)
                wp.atomic_add(accum,3*count,mass)

def run_fixture(device):
    import numpy as np
    # Unequal competing donors and receivers, including an isolated pin.
    n = 5
    pairs = np.array([[0,1],[0,2],[3,1],[2,3]],np.int32)
    amounts = np.array([.7,.3,.8,.1],np.float32)
    donor = np.array([.5,.4,.3,.2,0],np.float32)
    receiver = np.array([.5,.6,.4,.2,0],np.float32)
    ends=wp.array(pairs.ravel(),dtype=wp.int32,device=device)
    f=wp.array(amounts,dtype=float,device=device)
    caps=wp.array(np.concatenate([donor,receiver]),dtype=float,device=device)
    accum=wp.zeros(3*n+1,dtype=float,device=device)
    for _ in range(2):
        for phase in range(3):
            wp.launch(artistic_paired_exchange,dim=3*n+1 if phase==0 else len(pairs),
                inputs=[ends,f,caps,accum,n,phase],device=device)
        outgoing=np.bincount(pairs[:,0],weights=amounts,minlength=n)
        incoming=np.bincount(pairs[:,1],weights=amounts,minlength=n)
        expected=np.zeros(n)
        total=0.
        for (a,b),v in zip(pairs,amounts):
            mass=v*min(1.,donor[a]/outgoing[a],receiver[b]/incoming[b])
            expected[a]-=mass
            expected[b]+=mass
            total+=mass
        result=accum.numpy()
        np.testing.assert_allclose(result[2*n:3*n],expected,atol=2e-7,rtol=2e-5)
        np.testing.assert_allclose(result[3*n],total,atol=2e-7,rtol=2e-5)
        assert abs(float(result[2*n:3*n].sum()))<1e-6 and result[3*n-1]==0.

if __name__ == "__main__":
    import argparse
    import pathlib
    import re
    import shutil
    parser=argparse.ArgumentParser()
    parser.add_argument("--ptx-dir",type=pathlib.Path)
    args=parser.parse_args()
    if args.ptx_dir:
        wp.config.cuda_output="ptx"
        wp.config.ptx_target_arch=75
        wp.init()
        wp.set_module_options({"fast_math":False})
        run_fixture("cuda:0")
        for path in sorted(pathlib.Path(wp.config.kernel_cache_dir).rglob("*.ptx"),
                           key=lambda x:x.stat().st_mtime,reverse=True):
            matches=re.findall(r"\.entry\s+(artistic_paired_exchange\w*_cuda_kernel_forward)\s*\(",
                               path.read_text(errors="ignore"))
            if matches:
                args.ptx_dir.mkdir(parents=True,exist_ok=True)
                target=args.ptx_dir/"artistic_flux.ptx"
                if path.resolve()!=target.resolve():
                    shutil.copyfile(path,target)
                print(f'$env:BLENDER_SCULPT_D11_FLUX_PTX = "{target.resolve()}"')
                print(f'$env:BLENDER_SCULPT_D11_FLUX_KERNEL = "{matches[0]}"')
                break
        else:
            raise RuntimeError("artistic flux PTX entry not found")
    else:
        wp.init()
        run_fixture("cpu")
        if wp.is_cuda_available():
            run_fixture("cuda:0")
        else:
            print("CUDA unavailable; CPU reference only")
