"""D11.12 local pressure proposal; transport and writeback remain on CPU."""
import warp as wp

@wp.kernel
def artistic_pressure_local(
    relative: wp.array(dtype=wp.vec3), normal: wp.array(dtype=wp.vec3),
    weight: wp.array(dtype=float), compaction: wp.array(dtype=float),
    action: int, axis: wp.vec3, radius: float, dose: float, pressure: float,
    plane_height: float, yield_value: float, flow: float, compression: float,
    recovery: float, hardening: float,
    motion: wp.array(dtype=wp.vec3), drive: wp.array(dtype=float),
):
    i = wp.tid()
    motion[i] = wp.vec3(0.0, 0.0, 0.0)
    drive[i] = 0.0
    w = weight[i]
    if w > 0.0:
        q = wp.clamp(pressure, 0.0, 1.0)
        plastic = wp.max(0.0, q-yield_value)/(1.0-yield_value)/(1.0+hardening*wp.clamp(compaction[i],0.0,1.0))
        transport = plastic*flow
        plastic_sink = plastic*compression*(1.0-recovery)
        sink = plastic_sink+q*compression*recovery
        e = dose*w
        if action == 4:
            gap = wp.max(0.0, wp.dot(relative[i],axis)-plane_height)
            amount = gap*(1.0-wp.exp(-4.0*e/radius*sink))
            motion[i] = -axis*wp.min(gap,amount)
            drive[i] = w*transport*wp.clamp(gap/radius,0.0,1.0)
        elif action == 5:
            motion[i] = -axis*(e*sink)
            drive[i] = 0.5*w*transport
        else:
            motion[i] = -normal[i]*(1.5*e*plastic_sink)
            drive[i] = 0.05*w*transport

def run_fixture(device):
    import numpy as np
    p = wp.array([[0,0,0],[0,0,0],[0,0,0]],dtype=wp.vec3,device=device)
    n = wp.array([[0,0,1]]*3,dtype=wp.vec3,device=device)
    w = wp.array([1.,.5,0.],dtype=float,device=device)
    c = wp.array([0.,.5,0.],dtype=float,device=device)
    out = wp.zeros(3,dtype=wp.vec3,device=device)
    drive = wp.zeros(3,dtype=float,device=device)
    for action in (4,5,6):
        wp.launch(artistic_pressure_local,dim=3,inputs=[
            p,n,w,c,action,wp.vec3(0.,0.,1.),1.,.04,1.,-.08,.14,.35,.25,.04,.65],
            outputs=[out,drive],device=device)
        plastic = 1./(1.+.65*np.array([0.,.5,0.]))
        transport = plastic*.35
        ps = plastic*.25*.96
        sink = ps+.25*.04
        e = .04*np.array([1.,.5,0.])
        expected = -.08*(-np.expm1(-4.*e*sink)) if action==4 else -e*sink if action==5 else -1.5*e*ps
        d = np.array([1.,.5,0.])*transport*(.08 if action==4 else .5 if action==5 else .05)
        np.testing.assert_allclose(out.numpy()[:,2],expected,atol=2e-7,rtol=2e-5)
        np.testing.assert_allclose(out.numpy()[:,:2],0.,atol=1e-8)
        np.testing.assert_allclose(drive.numpy(),d,atol=2e-7,rtol=2e-5)

if __name__ == "__main__":
    import argparse
    import pathlib
    import re
    import shutil
    parser = argparse.ArgumentParser()
    parser.add_argument("--ptx-dir",type=pathlib.Path)
    args = parser.parse_args()
    if args.ptx_dir:
        wp.config.cuda_output = "ptx"
        wp.config.ptx_target_arch = 75
        wp.init()
        wp.set_module_options({"fast_math": False})
        run_fixture("cuda:0")
        # Export the actual generated symbol, not an assumed hash.
        cache = pathlib.Path(wp.config.kernel_cache_dir)
        for path in sorted(cache.rglob("*.ptx"),key=lambda x:x.stat().st_mtime,reverse=True):
            matches = re.findall(r"\.entry\s+(artistic_pressure_local\w*_cuda_kernel_forward)\s*\(",path.read_text(errors="ignore"))
            if matches:
                args.ptx_dir.mkdir(parents=True,exist_ok=True)
                target = args.ptx_dir/"artistic_pressure.ptx"
                if path.resolve()!=target.resolve():
                    shutil.copyfile(path,target)
                print(f'$env:BLENDER_SCULPT_D11_WARP_PTX = "{target.resolve()}"')
                print(f'$env:BLENDER_SCULPT_D11_WARP_KERNEL = "{matches[0]}"')
                break
        else:
            raise RuntimeError("artistic pressure PTX entry not found")
    else:
        wp.init()
        run_fixture("cpu")
        if wp.is_cuda_available():
            run_fixture("cuda:0")
        else:
            print("CUDA unavailable; CPU reference only")
