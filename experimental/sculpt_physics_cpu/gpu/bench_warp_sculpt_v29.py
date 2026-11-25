"""V29 compliant XPBD sculpt-grid validation, standalone Warp prototype.
Not native Blender, not the V28 conservative material solver.
Examples:
 python bench_warp_sculpt_v29.py --device cpu --side 16 --dabs 12
 python bench_warp_sculpt_v29.py --device cuda --side 16 --dabs 12 --parity
 python bench_warp_sculpt_v29.py --device cuda --side 256 --dabs 50 --graph
"""
import argparse
import json
import time
import numpy as np
from warp_xpbd_local import LocalXpbdSession
from warp_granular_probe import wp

def run(device,side,dabs,graph,parity):
    if not 8<=side<=512 or not 3<=dabs<=200:
        raise ValueError("side 8..512; dabs 3..200")
    if (device=="cpu" or parity) and side>24:
        raise ValueError("CPU oracle limited to side <=24")
    if graph and device!="cuda":
        raise ValueError("graph replay requires CUDA")
    y,x=np.mgrid[0:side,0:side].astype(np.float32)/(side-1)
    base=np.stack((x,y,.025*np.sin(8*x)*np.cos(7*y)),axis=-1).reshape(-1,3)
    ids=np.arange(side*side,dtype=np.int32).reshape(side,side)
    edges=np.concatenate([np.stack((ids[:,:-1],ids[:,1:]),-1).reshape(-1,2),
                          np.stack((ids[:-1,:],ids[1:,:]),-1).reshape(-1,2),
                          np.stack((ids[:-1,:-1],ids[1:,1:]),-1).reshape(-1,2)])
    inv=np.ones(len(base),dtype=np.float32)
    inv[(x.reshape(-1)<.12)|(y.reshape(-1)<.12)|(x.reshape(-1)>.88)|(y.reshape(-1)>.88)]=0
    pinned=np.flatnonzero(inv==0)
    mobile=np.flatnonzero(inv>0)
    start=time.perf_counter()
    session=LocalXpbdSession(base,inv,edges,device=device,target_mode="compliant",
                             compliance=1e-5,graph_capture=graph)
    if device=="cuda":wp.synchronize_device("cuda:0")
    initialization=(time.perf_counter()-start)*1000
    oracle=LocalXpbdSession(base,inv,edges,device="cpu",target_mode="compliant",
                            compliance=1e-5) if parity else None
    elapsed=[]
    prior=base[mobile].copy()
    max_error=0.0
    for k in range(dabs+2):
        center=np.array([.5+.18*np.sin(k*.35),.5+.13*np.cos(k*.35)],dtype=np.float32)
        distance=np.linalg.norm(base[mobile,:2]-center,axis=1)
        weight=np.maximum(0,1-(distance/.27)**2)**2
        selected=weight>0
        target_ids=mobile[selected]
        if not len(target_ids):raise RuntimeError("empty brush fixture")
        # identical absolute goals for both implementations; native writeback
        # and pressure/falloff evaluation are outside this prototype.
        goals=base[target_ids].copy()
        goals[:,2]+=.035*weight[selected]
        begin=time.perf_counter()
        session.set_targets(target_ids,goals,target_compliance=1e-6)
        for _ in range(2):session.solve(dt=1/120,iterations=5)
        current=session.read_positions(mobile)
        fixed=session.read_positions(pinned)
        if device=="cuda":wp.synchronize_device("cuda:0")
        duration=(time.perf_counter()-begin)*1000
        if k>=2:elapsed.append(duration)
        if not np.isfinite(current).all() or not np.array_equal(fixed,base[pinned]):
            raise RuntimeError("nonfinite output or moved pinned vertex")
        if oracle is not None:
            oracle.set_targets(target_ids,goals,target_compliance=1e-6)
            for _ in range(2):oracle.solve(dt=1/120,iterations=5)
            expected=oracle.read_positions(mobile)
            error=float(np.max(np.linalg.norm(expected-current,axis=1)))
            max_error=max(max_error,error)
            if error>2e-4:raise RuntimeError(f"CPU/Warp position error {error}")
        prior=current
    return dict(device=session.device,vertices=len(base),bonds=len(edges),
                dabs=dabs,initialization_ms=initialization,
                dab_ms_p50=float(np.percentile(elapsed,50)),
                dab_ms_p95=float(np.percentile(elapsed,95)),
                max_cpu_oracle_error=max_error if parity else None,
                max_displacement=float(np.max(np.linalg.norm(prior-base[mobile],axis=1))),
                pinned_vertices=len(pinned),transfer_bytes=session.transfer_bytes,
                graph_stats=session.graph_stats,sparse_allocations=session.sparse_device_allocations,
                scope="resident standalone XPBD; includes full mobile/pinned readback; no Blender integration")

if __name__=="__main__":
    p=argparse.ArgumentParser()
    p.add_argument("--device",choices=("cpu","cuda"),default="cpu")
    p.add_argument("--side",type=int,default=16)
    p.add_argument("--dabs",type=int,default=12)
    p.add_argument("--graph",action="store_true")
    p.add_argument("--parity",action="store_true")
    a=p.parse_args()
    print(json.dumps(run(a.device,a.side,a.dabs,a.graph,a.parity),indent=2))
