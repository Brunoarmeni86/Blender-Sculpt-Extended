"""D12 Very Strong compact multiscale height relaxation, Warp 1.19.
Separate PTX module: never replace the validated A3 or Jacobi kernels.
The host supplies a connected, affine-detrended, dimensionless local chart.
"""
from pathlib import Path
import argparse
import re
import shutil
import tempfile

import numpy as np
import warp as wp


@wp.kernel
def sculpt_smooth_multiscale(
    points: wp.array(dtype=wp.vec3),
    mass: wp.array(dtype=float),
    weight: wp.array(dtype=float),
    grid_mass: wp.array(dtype=float),
    grid_height: wp.array(dtype=float),
    field: wp.array(dtype=float),
    support: wp.array(dtype=float),
    output: wp.array(dtype=float),
    phase: int,
):
    k = wp.tid()
    if phase == 0:
        grid_mass[k] = 0.0
        grid_height[k] = 0.0
        field[k] = 0.0
        support[k] = 0.0
    elif phase == 1:
        p = points[k]
        x = p[0] * 12.0 + 16.0
        y = p[1] * 12.0 + 16.0
        ix = int(x)
        iy = int(y)
        for j in range(2):
            for i in range(2):
                wx = x - float(ix)
                wy = y - float(iy)
                if i == 0:
                    wx = 1.0 - wx
                if j == 0:
                    wy = 1.0 - wy
                w = mass[k] * wx * wy
                idx = (iy+j)*33+ix+i
                wp.atomic_add(grid_mass, idx, w)
                wp.atomic_add(grid_height, idx, w*p[2])
    elif phase == 2:
        x = k % 33
        y = k // 33
        m1 = float(0.0)
        h1 = float(0.0)
        m2 = float(0.0)
        h2 = float(0.0)
        for j in range(-5, 6):
            for i in range(-5, 6):
                a = x + i
                b = y + j
                if a >= 0 and a < 33 and b >= 0 and b < 33:
                    idx = b*33+a
                    w2 = float(6-wp.abs(i))*float(6-wp.abs(j))
                    m2 += grid_mass[idx]*w2
                    h2 += grid_height[idx]*w2
                    if wp.abs(i) <= 2 and wp.abs(j) <= 2:
                        w1 = float(3-wp.abs(i))*float(3-wp.abs(j))
                        m1 += grid_mass[idx]*w1
                        h1 += grid_height[idx]*w1
        if m2 > 1.0e-15:
            broad = h2/m2
            fine = broad
            if m1 > 1.0e-15:
                fine = h1/m1
            support[k] = 1.0
            field[k] = 0.25*fine+0.75*broad
    else:
        p = points[k]
        x = p[0]*12.0+16.0
        y = p[1]*12.0+16.0
        ix = int(x)
        iy = int(y)
        val = float(0.0)
        total = float(0.0)
        for j in range(2):
            for i in range(2):
                wx = x-float(ix)
                wy = y-float(iy)
                if i == 0:
                    wx = 1.0-wx
                if j == 0:
                    wy = 1.0-wy
                idx = (iy+j)*33+ix+i
                w = wx*wy*support[idx]
                val += w*field[idx]
                total += w
        output[k] = 0.0
        if total > 1.0e-15:
            output[k] = wp.clamp((val/total-p[2])*0.85,-0.055,0.055)*weight[k]


def cpu_reference(points, mass, weight):
    gm = np.zeros((33,33),dtype=np.float64)
    gh = np.zeros_like(gm)
    for (x,y,h),m in zip(points,mass):
        x,y=x*12+16,y*12+16
        ix,iy=int(x),int(y)
        for j in range(2):
            for i in range(2):
                w=m*(x-ix if i else 1-(x-ix))*(y-iy if j else 1-(y-iy))
                gm[iy+j,ix+i]+=w
                gh[iy+j,ix+i]+=w*h
    field=np.zeros_like(gm)
    support=np.zeros_like(gm)
    for y in range(33):
        for x in range(33):
            m1=h1=m2=h2=0.
            for j in range(-5,6):
                for i in range(-5,6):
                    a,b=x+i,y+j
                    if 0<=a<33 and 0<=b<33:
                        w=(6-abs(i))*(6-abs(j))
                        m2+=gm[b,a]*w
                        h2+=gh[b,a]*w
                        if abs(i)<=2 and abs(j)<=2:
                            w=(3-abs(i))*(3-abs(j))
                            m1+=gm[b,a]*w
                            h1+=gh[b,a]*w
            if m2>1e-15:
                broad=h2/m2
                field[y,x]=.25*(h1/m1 if m1>1e-15 else broad)+.75*broad
                support[y,x]=1.
    out=[]
    for (x,y,h),factor in zip(points,weight):
        x,y=x*12+16,y*12+16
        ix,iy=int(x),int(y)
        val=total=0.
        for j in range(2):
            for i in range(2):
                w=support[iy+j,ix+i]*(x-ix if i else 1-(x-ix))*(y-iy if j else 1-(y-iy))
                val+=w*field[iy+j,ix+i]
                total+=w
        out.append(np.clip((val/total-h)*.85,-.055,.055)*factor if total>1e-15 else 0.)
    return np.array(out)


def fixture(device):
    x,y=np.meshgrid(np.linspace(-.9,.9,45),np.linspace(-.9,.9,45))
    x,y=x.ravel(),y.ravel()
    points=np.column_stack((x,y,.18*np.exp(-(x*x+y*y)/.045))).astype(np.float32)
    mass=np.ones(len(points),dtype=np.float32)
    weight=np.clip(1.-np.hypot(x,y),0.,1.).astype(np.float32)
    weight[0]=0.
    args=[wp.array(points,dtype=wp.vec3,device=device),
          wp.array(mass,dtype=float,device=device),
          wp.array(weight,dtype=float,device=device)]
    arrays=[wp.zeros(1089,dtype=float,device=device) for _ in range(4)]
    out=wp.zeros(len(points),dtype=float,device=device)
    for phase in range(4):
        wp.launch(sculpt_smooth_multiscale,dim=1089 if phase in (0,2) else len(points),
                  inputs=args+arrays+[out,phase],device=device)
    wp.synchronize()
    actual=out.numpy()
    expected=cpu_reference(points,mass,weight)
    np.testing.assert_allclose(actual,expected,rtol=3e-4,atol=2e-6)
    assert actual[len(points)//2]<-.02
    assert actual[0]==0.
    assert np.isfinite(actual).all()
    print("MULTISCALE_OK",device,"center",actual[len(points)//2])


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if not args.output:
        wp.init()
        fixture("cpu")
        if wp.is_cuda_available():
            fixture("cuda:0")
        else:
            print("CUDA unavailable; CPU reference only")
        return
    if str(wp.__version__)!="1.19.0":
        raise RuntimeError("Export requires Warp 1.19.0")
    wp.config.cuda_output="ptx"
    wp.config.ptx_target_arch=75
    with tempfile.TemporaryDirectory(prefix="bse-smooth-") as directory:
        wp.config.kernel_cache_dir=directory
        wp.init()
        wp.set_module_options({"fast_math":False})
        fixture("cuda:0")
        for path in Path(wp.config.kernel_cache_dir).rglob("*.ptx"):
            matches=re.findall(r"\.entry\s+(sculpt_smooth_multiscale\w*_cuda_kernel_forward)\s*\(",
                               path.read_text(errors="ignore"))
            if len(matches)==1:
                args.output.mkdir(parents=True,exist_ok=True)
                target=args.output/"blender_smooth_multiscale.sm75.ptx"
                shutil.copyfile(path,target)
                target.with_suffix(".symbols").write_text(matches[0]+"\n",encoding="ascii")
                print("[OK]",target.resolve(),matches[0])
                return
        raise RuntimeError("Multiscale CUDA entry not found")


if __name__=="__main__":
    main()
