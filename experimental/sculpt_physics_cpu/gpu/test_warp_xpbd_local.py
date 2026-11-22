"""CPU-oracle regression for the Warp edge-colored local XPBD prototype.

python gpu/test_warp_xpbd_local.py --device cpu
python gpu/test_warp_xpbd_local.py --device cuda  # requires Warp + GPU
"""
import argparse
import numpy as np

from warp_xpbd_local import LocalXpbdSession


def run(device):
    vertices=np.array([[0.0,0.0,0.0],[1.0,0.0,0.0],[2.0,0.0,0.0]],
                      dtype=np.float32)
    masses=np.array([0.0,1.0,1.0],dtype=np.float32)
    edges=np.array([[0,1],[1,2]],dtype=np.int32)
    solver=LocalXpbdSession(vertices,masses,edges,device=device)
    assert solver.device==device
    assert solver.num_colors==2 # adjacent edges cannot update simultaneously
    baseline=solver.transfer_bytes
    solver.set_targets([2],[[2.6,0,0]])
    solver.solve(dt=1.0/60.0,iterations=8)
    out=solver.read_positions([0,1,2])
    assert np.allclose(out[0],vertices[0],atol=2.0e-5)
    assert out[1,0]>1.0 and out[2,0]>2.0
    assert np.allclose(out[2], [2.6,0,0], atol=1e-6), "last color moved hard brush target"
    assert np.allclose(vertices[:,0],[0,1,2]) # input remains unchanged
    assert solver.transfer_bytes[0]==baseline[0] # geometry never re-uploaded

    solver.set_targets([2],[[2.2,0,0]])
    solver.solve(iterations=4)
    second=solver.read_positions([1,2])
    assert np.isfinite(second).all()
    assert np.allclose(second[1], [2.2,0,0],atol=1e-6)
    assert solver.transfer_bytes[0]==baseline[0]

    before=solver.read_positions([0,1,2])
    for ids,goals,exc in [
        ([2,2],[[3,0,0],[3,0,0]],ValueError),
        ([0],[[0,0,0]],ValueError), # pinned particle
        ([4],[[0,0,0]],IndexError),
        ([2],[[float("nan"),0,0]],ValueError),
    ]:
        try: solver.set_targets(ids,goals)
        except exc: pass
        else: raise AssertionError("invalid target accepted")
        assert np.allclose(before,solver.read_positions([0,1,2]),atol=1e-5)

    try: solver.solve(dt=0)
    except ValueError: pass
    else: raise AssertionError("invalid dt accepted")
    assert solver.read_positions([]).shape==(0,3)
    print("colored XPBD local prototype passed:",device,solver.transfer_bytes)
    return out


def test_rest_length_from_native_world(device):
    # Plastic/rest lengths must come from the authoritative C++ World and
    # must not be silently rebuilt from the deformed geometry.
    p=np.array([[0.0,0.0,0.0],[1.0,0.0,0.0]],dtype=np.float32)
    inv=np.array([0.0,1.0],dtype=np.float32)
    edges=np.array([[0,1]],dtype=np.int32)
    default=LocalXpbdSession(p,inv,edges,device=device)
    default.solve(iterations=5)
    assert np.allclose(default.read_positions([1]),[[1.0,0,0]],atol=1e-6)
    stored=LocalXpbdSession(p,inv,edges,rest_lengths=[0.6],device=device)
    stored.solve(iterations=5)
    assert np.allclose(stored.read_positions([1]),[[0.6,0,0]],atol=2e-5)
    for bad in ([float("nan")],[-1.0],[0.0],[],[0.6,0.7]):
        try: LocalXpbdSession(p,inv,edges,rest_lengths=bad,device=device)
        except ValueError: pass
        else: raise AssertionError("invalid physical rest length accepted")


def test_gpu_parity():
    p=np.array([[0,0,0],[1,0,0],[2,0,0]],dtype=np.float32)
    inv=np.array([0,1,1],dtype=np.float32)
    e=np.array([[0,1],[1,2]],dtype=np.int32)
    cpu=LocalXpbdSession(p,inv,e,device="cpu")
    gpu=LocalXpbdSession(p,inv,e,device="cuda")
    for target in [2.6,2.2,2.9]:
        for session in (cpu,gpu):
            session.set_targets([2],[[target,0,0]])
            session.solve(iterations=8)
        assert np.allclose(cpu.read_positions([0,1,2]),
                           gpu.read_positions([0,1,2]),
                           atol=3e-4,rtol=3e-4)


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--device",choices=("cpu","cuda"),default="cpu")
    args=parser.parse_args()
    run(args.device)
    test_rest_length_from_native_world(args.device)
    if args.device=="cuda":
        test_gpu_parity()
