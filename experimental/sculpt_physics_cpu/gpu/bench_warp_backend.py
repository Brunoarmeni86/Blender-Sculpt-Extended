"""Reproducible microbenchmark of standalone resident GPU prototypes.

Examples (run on an NVIDIA machine with warp-lang installed):
    python gpu/bench_warp_backend.py --solver xpbd --device cuda --particles 20000 --dabs 50
    python gpu/bench_warp_backend.py --solver granular --device cuda --particles 10000 --dabs 50

CPU mode is a slow Python numerical oracle, NOT the optimized C++ Blender
CPU backend. Compare accuracy and transfer volume; NOT speedups vs Blender.
"""
from __future__ import annotations
import argparse
import json
import time
import numpy as np

from warp_resident_contact import ResidentContactSession
from warp_xpbd_local import LocalXpbdSession
from warp_granular_probe import wp


def sync(device):
    if device=="cuda":
        wp.synchronize_device("cuda:0")


def run(solver,device,particles,dabs,iterations):
    if device=="cpu" and particles>256:
        raise ValueError("Python CPU oracle capped at 256 particles")
    if particles<4 or particles>2_000_000 or dabs<3 or dabs>1000:
        raise ValueError("invalid benchmark size")
    if solver=="xpbd":
        base=np.zeros((particles,3),dtype=np.float32)
        base[:,0]=np.arange(particles,dtype=np.float32)*0.1
        inv=np.ones(particles,dtype=np.float32)
        inv[0]=0.0
        edges=np.stack((np.arange(particles-1,dtype=np.int32),
                        np.arange(1,particles,dtype=np.int32)),axis=1)
        start=time.perf_counter()
        session=LocalXpbdSession(base,inv,edges,device=device)
        init_ms=(time.perf_counter()-start)*1000.0
        center=particles//2
        def dab(k):
            session.set_targets([center],[[base[center,0],0.01*(1+k%3),0.0]])
            session.solve(iterations=iterations)
            return session.read_positions([center,center+1])
    else:
        side=int(np.ceil(particles**(1.0/3.0)))
        xyz=np.stack(np.unravel_index(np.arange(particles), (side,side,side)),
                     axis=1).astype(np.float32)*0.085
        inv=np.ones(particles,dtype=np.float32)
        radii=np.full(particles,0.05,dtype=np.float32)
        start=time.perf_counter()
        session=ResidentContactSession(xyz,inv,radii,device=device)
        init_ms=(time.perf_counter()-start)*1000.0
        def dab(k):
            session.step(iterations=iterations)
            return session.read_positions([0,1])
    sync(device)
    for k in range(2):
        output=dab(k)
        if not np.isfinite(output).all(): raise RuntimeError("warmup diverged")
    sync(device)
    timings=[]
    for i in range(dabs):
        start=time.perf_counter()
        output=dab(i+2)
        sync(device)
        if not np.isfinite(output).all(): raise RuntimeError("non-finite benchmark")
        timings.append((time.perf_counter()-start)*1000.0)
    timings=np.array(timings,dtype=np.float64)
    stats=session.transfer_bytes if solver=="xpbd" else session.stats
    return {
        "device":device, "solver":solver, "particles":particles,
        "iterations":iterations, "measured_dabs":dabs,
        "initialization_ms":round(init_ms,4),
        "dab_ms_p50":round(float(np.percentile(timings,50)),4),
        "dab_ms_p95":round(float(np.percentile(timings,95)),4),
        "transfer_counters":str(stats),
        "scope":"standalone Python/Warp proxy, no Blender PBVH/CCG/Undo/viewport"
    }


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--device",choices=("cpu","cuda"),default="cpu")
    parser.add_argument("--solver",choices=("xpbd","granular"),default="xpbd")
    parser.add_argument("--particles",type=int,default=64)
    parser.add_argument("--dabs",type=int,default=10)
    parser.add_argument("--iterations",type=int,default=4)
    args=parser.parse_args()
    print(json.dumps(run(args.solver,args.device,args.particles,
                         args.dabs,args.iterations),indent=2))
