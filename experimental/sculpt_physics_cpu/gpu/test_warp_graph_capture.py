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
            session.set_targets(ids, goals if ids else np.empty((0,3), dtype=np.float32),
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


def check_buffer_growth(device: str) -> None:
    # A grown target buffer must NOT replay a graph with stale pointers.
    p = np.stack((np.arange(12, dtype=np.float32),
                  np.zeros(12, dtype=np.float32),
                  np.zeros(12, dtype=np.float32)), axis=1)
    mass = np.ones(12, dtype=np.float32)
    mass[0] = 0
    edges = np.stack((np.arange(11, dtype=np.int32),
                      np.arange(1, 12, dtype=np.int32)), axis=1)
    plain = LocalXpbdSession(p, mass, edges, device=device)
    graph = LocalXpbdSession(p, mass, edges, device=device, graph_capture=True)
    for offset in (0.1, 0.2):
        for session in (plain, graph):
            session.set_targets([11], [[11 + offset, 0, 0]])
            session.solve(iterations=2)
        assert np.allclose(plain.read_positions([10, 11]),
                           graph.read_positions([10, 11]), atol=3e-5)
    assert graph.graph_stats[2] == 1, graph.graph_stats

    many = list(range(2, 12))  # ten movable targets; previous capacity was 8
    goals = [[float(i) + 0.25, 0.0, 0.0] for i in many]
    for session in (plain, graph):
        session.set_targets(many, goals)
    assert graph.graph_stats[2] == 0, (
        "cached graph still references target buffers after reallocation")
    for session in (plain, graph):
        session.solve(iterations=2)
    assert np.allclose(plain.read_positions(list(range(12))),
                       graph.read_positions(list(range(12))), atol=3e-5)
    print("Warp XPBD graph pointer invalidation passed:", device)


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
    check_buffer_growth(args.device)
