"""Opt-in Warp CPU/CUDA solve-graph correctness and cache regressions.

Unlike a simple NumPy oracle, --device warp-cpu runs captured real Warp
kernels in CI. --device cuda requires an NVIDIA GPU and stays opt-in.
No native Blender/PBVH/Multires/Undo integration is implied.
"""
from __future__ import annotations
import argparse
import numpy as np
from warp_xpbd_local import LocalXpbdSession


def check(device: str, mode: str) -> None:
    positions = np.array([[0, 0, 0], [1, 0, 0], [2, 0, 0],
                          [3, 0, 0], [4, 0, 0]], dtype=np.float32)
    mass = np.array([0, 1, 1, 1, 1], dtype=np.float32)
    edges = np.array([[0, 1], [1, 2], [2, 3], [3, 4]], dtype=np.int32)
    direct = LocalXpbdSession(positions, mass, edges, device=device,
                              compliance=0.001, target_mode=mode)
    captured = LocalXpbdSession(positions, mass, edges, device=device,
                                compliance=0.001, target_mode=mode,
                                graph_capture=True)
    # Equal-count targets with changing vertex IDs and goals MUST replay
    # against live buffers, never stale captured values.
    steps = [
        ([4], [[4.5, 0.15, 0]], 4, 1/60),
        ([3], [[3.2, 0.18, 0]], 4, 1/60),
        ([4], [[4.2, 0.21, 0]], 4, 1/60),
        ([3, 4], [[3.3, 0.12, 0], [4.4, 0.3, 0]], 4, 1/60),
        ([2, 4], [[2.5, 0.22, 0], [4.6, 0.1, 0]], 4, 1/60),
        ([4], [[4.1, 0.25, 0]], 2, 1/120),
        ([3], [[3.4, 0.1, 0]], 2, 1/120),
        ([2], [[2.2, 0.05, 0]], 4, 1/60),
        ([], [], 4, 1/60),
    ]
    for ids, goals, iters, dt in steps:
        for session in (direct, captured):
            session.set_targets(ids, goals,
                                target_compliance=0.0005 if mode == "compliant" else 0.0)
            session.solve(iterations=iters, dt=dt)
        a = direct.read_positions([0, 1, 2, 3, 4])
        b = captured.read_positions([0, 1, 2, 3, 4])
        assert np.isfinite(b).all()
        assert np.allclose(a, b, atol=3e-5, rtol=3e-5), (mode, ids, a, b)
        assert np.array_equal(b[0], positions[0]), "pinned vertex moved"

    built, replayed, cached = captured.graph_stats
    assert built >= 2 and replayed >= 4 and cached >= 1, captured.graph_stats
    assert direct.graph_stats == (0, 0, 0)
    print("Warp captured XPBD parity:", device, mode, captured.graph_stats)


def reject_bad_mode() -> None:
    p = np.array([[0,0,0],[1,0,0]], dtype=np.float32)
    inv = np.array([0,1], dtype=np.float32)
    bonds = np.array([[0,1]], dtype=np.int32)
    try:
        LocalXpbdSession(p, inv, bonds, device="cpu", graph_capture=True)
    except ValueError:
        pass
    else:
        raise AssertionError("NumPy oracle unexpectedly accepted Warp graph capture")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", choices=("warp-cpu", "cuda"), default="warp-cpu")
    args = parser.parse_args()
    reject_bad_mode()
    check(args.device, "hard")
    check(args.device, "compliant")
