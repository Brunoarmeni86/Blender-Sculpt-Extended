# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit device smoke test; CUDA request never silently falls back."""
import argparse
import warp as wp
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
    print("D14_VOLUME_DEVICE_FIXTURES_OK",args.device,args.solver)
if __name__=="__main__":
    main()
