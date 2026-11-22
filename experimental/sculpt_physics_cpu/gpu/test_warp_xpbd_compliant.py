"""Parity and numerical invariants for *compliant* Warp XPBD brush targets.

Run without Warp: python gpu/test_warp_xpbd_compliant.py --device cpu
Compile actual Warp JIT kernels: --device warp-cpu
NVIDIA hardware parity (not run in CI): --device cuda

The legacy target_mode='hard' remains unchanged and is tested separately.
"""
import argparse
import numpy as np

from warp_xpbd_local import LocalXpbdSession


def matches(a, b, tol=3.0e-4):
    return np.allclose(a, b, atol=tol, rtol=tol)


def sample(device):
    p=np.array([[0,0,0],[1,0,0],[2,0,0]],dtype=np.float32)
    inv=np.array([0,1,1],dtype=np.float32)
    edges=np.array([[0,1],[1,2]],dtype=np.int32)
    solver=LocalXpbdSession(
        p, inv, edges, compliance=[0.0,1.0e-5],
        target_mode="compliant", device=device)
    assert solver.num_colors == 2
    solver.set_targets([2],[[2,0.8,0]],target_compliance=1.0e-4)
    solver.solve(iterations=12)
    result=solver.read_positions([0,1,2])
    assert matches(result[0],p[0])
    assert 0.0 < result[2,1] < 0.8
    assert result[1,1] > 0.0
    assert np.isfinite(result).all()
    history=[result.copy()]

    # Compliant goals are not forcefully re-pinned after bond constraints.
    # Repeated dabs keep resident geometry, but reset XPBD multipliers.
    baseline_transfer=solver.transfer_bytes[0]
    for goal_y in (0.5,0.9,0.2,0.4):
        solver.set_targets([1,2],[[1,0.1,0],[2,goal_y,0]],
                           target_compliance=[1.0e-6,1.0e-4])
        solver.solve(iterations=7)
        staged=solver.read_positions([0,1,2])
        assert np.isfinite(staged).all()
        history.append(staged.copy())
    assert solver.transfer_bytes[0] == baseline_transfer

    prev=solver.read_positions([0,1,2])
    for bad in ([-1.0],[np.nan],[1.0e-4,0.0,0.0]):
        try:
            solver.set_targets([2],[[2,0.3,0]],target_compliance=bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid compliance was accepted")
        assert matches(prev, solver.read_positions([0,1,2]),1e-6)
    for ids,goals in [([2,2],[[2,0,0],[2,0,0]]),
                       ([0],[[0,0.1,0]])]:
        try: solver.set_targets(ids,goals,target_compliance=1e-4)
        except ValueError: pass
        else: raise AssertionError("invalid target set accepted")
    return np.stack(history)


def isolated_compliant_solution(device):
    # Closed-form one-particle XPBD. The per-substep multiplier eliminates
    # per-iteration over-application of the brush force.
    p=np.array([[1.0,0.0,0.0]],dtype=np.float32)
    inv=np.array([1.0],dtype=np.float32)
    e=np.empty((0,2),dtype=np.int32)
    h=1.0/60.0
    compliance=1.0e-4
    alpha=compliance/(h*h)
    expected=(1.0*alpha + 1.5)/(1.0+alpha)
    solver=LocalXpbdSession(p,inv,e,target_mode="compliant",device=device)
    solver.set_targets([0],[[1.5,0,0]],target_compliance=compliance)
    solver.solve(dt=h,iterations=8)
    got=float(solver.read_positions([0])[0,0])
    assert abs(got-expected)<1e-5,(got,expected)

    # A zero-compliance target with NO bonds converges to the exact goal.
    solver.set_targets([0],[[1.9,0,0]],target_compliance=0.0)
    solver.solve(dt=h,iterations=5)
    assert matches(solver.read_positions([0]),[[1.9,0,0]],1e-5)


def parity():
    cpu=sample("cpu")
    warp=sample("warp-cpu")
    for step,(a,b) in enumerate(zip(cpu,warp)):
        assert matches(a,b),(step,a,b)
    isolated_compliant_solution("cpu")
    isolated_compliant_solution("warp-cpu")


def cuda_parity():
    cpu=sample("cpu")
    cuda=sample("cuda")
    assert matches(cpu,cuda)
    isolated_compliant_solution("cuda")


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--device",choices=("cpu","warp-cpu","cuda"),default="cpu")
    args=parser.parse_args()
    if args.device=="warp-cpu":
        parity()
    elif args.device=="cuda":
        cuda_parity()
    else:
        sample("cpu")
        isolated_compliant_solution("cpu")
    print("Compliant XPBD target reference passed:",args.device)
