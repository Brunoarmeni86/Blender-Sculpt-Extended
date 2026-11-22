"""Regression and transfer-budget checks for persistent Warp contact buffers.

Runs with NO CUDA or Warp installed:
  python gpu/test_warp_resident_contact.py --device cpu
Optional GPU parity:
  python gpu/test_warp_resident_contact.py --device cuda

These tests validate the standalone contact-projection behavior, not the
native Blender viewport or CPU World/Undo transaction.
"""
import argparse
import numpy as np

from warp_resident_contact import ResidentContactSession
from warp_granular_probe import project


def equal(a, b, atol=2.0e-5):
    return np.allclose(a, b, atol=atol, rtol=1.0e-5)


def smoke(device):
    points = np.array([[0.0,0.0,0.0], [0.6,0.0,0.0], [3.0,0.0,0.0]],
                      dtype=np.float32)
    masses = np.array([1.0,1.0,0.0],dtype=np.float32)
    radii = np.array([0.5,0.5,0.5],dtype=np.float32)
    session = ResidentContactSession(points,masses,radii,device=device)
    assert session.particle_count == 3
    assert session.device == device
    assert equal(session.read_positions([0,1]),points[:2])
    if device == "cuda":
        baseline_upload = session.stats.initial_upload_bytes
        assert baseline_upload == points.nbytes + masses.nbytes + radii.nbytes

    session.step(iterations=3)
    assert equal(points[1],[0.6,0,0]), "input was mutated"
    after = session.read_positions([0,1])
    assert after[0,0] < 0.0 < after[1,0]
    assert after[1,0] > 0.6
    assert equal(session.read_positions([2]),points[2:3])  # pinned
    full_downloads_before = session.stats.full_download_bytes
    session.step(iterations=2)
    assert session.stats.full_download_bytes == full_downloads_before
    assert equal(session.read_positions([2]),points[2:3])

    session.update_positions([0,1],[[0,0,0],[0.6,0,0]])
    edited = session.read_positions([0,1])
    assert equal(edited,points[:2])
    session.step(iterations=3, cohesion=0.0)
    assert session.read_positions([0,1])[0,0] < 0
    session.update_positions([0,1],[[0,0,0],[1.2,0,0]])
    session.step(iterations=2, cohesion=0.2)
    wet = session.read_positions([0,1])
    assert wet[0,0] > 0 and wet[1,0] < 1.2

    previous = session.read_positions([0,1])
    for ids, edits, error in [
        ([0,0],[[0,0,0],[0,0,0]],ValueError),
        ([4],[[0,0,0]],IndexError),
        ([0],[[np.nan,0,0]],ValueError),
        ([0],[[0,0]],ValueError),
    ]:
        rejected = False
        try:
            session.update_positions(ids,edits)
        except error:
            rejected = True
        assert rejected, (ids,error)
        assert equal(session.read_positions([0,1]),previous)
    for bad in [0,65]:
        try: session.step(iterations=bad)
        except ValueError: pass
        else: raise AssertionError("invalid iteration count accepted")
    assert equal(session.read_positions([]),np.empty((0,3),dtype=np.float32))
    print("resident contact tests passed:",device,session.stats)


def cpu_oracle_parity():
    p=np.array([[0,0,0],[0.6,0,0],[2.5,0,0]],dtype=np.float32)
    m=np.array([1,1,0],dtype=np.float32)
    r=np.full(3,0.5,dtype=np.float32)
    expected=project(p,m,r,device="cpu",iterations=3)
    session=ResidentContactSession(p,m,r,device="cpu")
    session.step(iterations=3)
    assert equal(expected,session.read_positions())
    assert session.stats.initial_upload_bytes == 0


if __name__ == "__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--device",choices=("cpu","cuda"),default="cpu")
    args=parser.parse_args()
    smoke(args.device)
    if args.device=="cpu":
        cpu_oracle_parity()
