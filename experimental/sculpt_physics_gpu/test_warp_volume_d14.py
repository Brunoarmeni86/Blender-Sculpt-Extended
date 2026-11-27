# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit device smoke test; CUDA request never silently falls back."""
import argparse
import warp as wp
import numpy as np
import prototype_warp_volume_pbf_d14 as pbf
import prototype_warp_volume_mpm_d14 as mpm

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device",choices=("cpu","cuda:0"),default="cuda:0")
    parser.add_argument("--solver",choices=("both","pbf","mpm"),default="both")
    args=parser.parse_args()
    wp.init()
    if str(wp.__version__)!="1.19.0":
        raise RuntimeError("Pinned Warp 1.19.0 required")
    if args.device=="cuda:0" and not wp.is_cuda_available():
        raise RuntimeError("CUDA unavailable; no fallback")
    if args.solver in ("both","pbf"):
        pbf.fixture(args.device)
    if args.solver in ("both","mpm"):
        mpm.fixture(args.device)
    # The same float32 Warp implementation on both devices is a codegen/
    # backend comparison, not independent parity against the C++ oracle.
    if args.device=="cuda:0":
        x=np.array([[a*.1,b*.1,.2+c*.1] for a in range(3) for b in range(3) for c in range(3)],np.float32)
        if args.solver in ("both","pbf"):
            a=pbf.PBF(x,h=.2,mass=.001,rho0=.8,device="cpu")
            b=pbf.PBF(x,h=.2,mass=.001,rho0=.8,device="cuda:0")
            for _ in range(2):
                a.step(.001,gravity=(0.,0.,0.))
                b.step(.001,gravity=(0.,0.,0.))
            np.testing.assert_allclose(a.x.numpy(),b.x.numpy(),rtol=2e-4,atol=2e-5)
            np.testing.assert_allclose(a.v.numpy(),b.v.numpy(),rtol=2e-3,atol=2e-3)
        if args.solver in ("both","mpm"):
            # Nonuniform velocity produces actual stress and volume changes.
            v=np.zeros_like(x);v[:,0]=-.5*x[:,0]
            args_mpm=dict(dx=.2,particle_spacing=.1,origin=(-.8,-.8,-.8),n=16)
            a=mpm.MPM(x,v,device="cpu",**args_mpm)
            b=mpm.MPM(x,v,device="cuda:0",**args_mpm)
            for _ in range(5):
                a.step(.001,gravity=(0.,0.,0.))
                b.step(.001,gravity=(0.,0.,0.))
            for key in ("x","v","volume","stress","plastic"):
                np.testing.assert_allclose(getattr(a,key).numpy(),getattr(b,key).numpy(),rtol=2e-3,atol=2e-5)
        print("D14_WARP_CPU_CUDA_PARITY_OK")
    print("D14_VOLUME_DEVICE_FIXTURES_OK",args.device,args.solver)
if __name__=="__main__":
    main()
