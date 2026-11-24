"""Standalone NVIDIA Warp HashGrid contact prototype with NumPy CPU oracle.

NOT connected to Blender/PBVH, C++ World, Multires or Sculpt Layers.
Run CPU fallback (no Warp or CUDA required):
    python gpu/warp_granular_probe.py --device cpu --self-test
Optional Warp + CUDA:
    python gpu/warp_granular_probe.py --device cuda --self-test
"""
from __future__ import annotations
import argparse
import numpy as np

try:
    import warp as wp
except ImportError:
    wp = None

if wp is not None:
    @wp.kernel
    def _contact_jacobi(
        grid_id: wp.uint64,
        positions: wp.array(dtype=wp.vec3),
        inverse_masses: wp.array(dtype=wp.float32),
        radii: wp.array(dtype=wp.float32),
        separation: wp.float32,
        cohesion: wp.float32,
        cohesion_range: wp.float32,
        search_radius: wp.float32,
        result: wp.array(dtype=wp.vec3),
    ):
        i = wp.tid()
        p = positions[i]
        inv_i = inverse_masses[i]
        if inv_i <= 0.0:
            result[i] = p
            return
        query = wp.hash_grid_query(grid_id, p, search_radius)
        j = int(0)
        delta = wp.vec3(0.0, 0.0, 0.0)
        while wp.hash_grid_query_next(query, j):
            if i == j:
                continue
            inv_j = inverse_masses[j]
            inv_sum = inv_i + inv_j
            if inv_sum <= 0.0:
                continue
            offset = positions[j] - p
            dist = wp.length(offset)
            touching = radii[i] + radii[j]
            if dist >= touching * cohesion_range:
                continue
            direction = wp.vec3(1.0, 0.0, 0.0)
            if dist > 1.0e-12:
                direction = offset / dist
            elif i > j:
                direction = wp.vec3(-1.0, 0.0, 0.0)
            elif i > j:
                direction = wp.vec3(-1.0, 0.0, 0.0)
            signed_shift = 0.0
            if dist < touching:
                signed_shift = -(touching - dist) * separation
            else:
                signed_shift = (dist - touching) * cohesion
            delta = delta + direction * (signed_shift * inv_i / inv_sum)
        result[i] = p + delta


def _cpu_step(p, masses, radii, separation, cohesion, cohesion_range):
    # O(N^2) CPU oracle for tiny fixtures only: use C++ hash-grid for CPU scale.
    correction = np.zeros_like(p)
    for i in range(len(p)):
        if masses[i] <= 0:
            continue
        for j in range(len(p)):
            if i == j or masses[i] + masses[j] <= 0:
                continue
            offset = p[j] - p[i]
            distance = float(np.linalg.norm(offset))
            touching = float(radii[i] + radii[j])
            if distance >= touching * cohesion_range:
                continue
            if distance > 1.0e-12:
                direction = offset / distance
            else:
                direction = np.array([1.0 if i < j else -1.0, 0.0, 0.0])
            shift = (-(touching - distance) * separation if distance < touching
                     else (distance - touching) * cohesion)
            correction[i] += direction * (shift * masses[i] / (masses[i] + masses[j]))
    return p + correction


def project(positions, inverse_masses, radii, *, iterations=4,
            separation=0.85, cohesion=0.0, cohesion_range=1.25, device="auto"):
    """Out-of-place local contact projection. No mesh/UV writeback."""
    p = np.asarray(positions, dtype=np.float32)
    masses = np.asarray(inverse_masses, dtype=np.float32)
    radius = np.asarray(radii, dtype=np.float32)
    if (p.ndim != 2 or p.shape[1] != 3 or masses.shape != (len(p),)
            or radius.shape != (len(p),) or not np.isfinite(p).all()
            or not np.isfinite(masses).all() or not np.isfinite(radius).all()
            or np.any(masses < 0) or np.any(radius <= 0)
            or iterations < 1 or iterations > 64 or not 0 <= separation <= 1
            or not 0 <= cohesion <= 1 or not 1 <= cohesion_range <= 8):
        raise ValueError("Invalid granular inputs")
    if device not in ("auto", "cuda", "cpu"):
        raise ValueError("device must be auto, cpu or cuda")
    if len(p) == 0:
        return p.copy()
    gpu = (device == "cuda" or (device == "auto" and wp is not None
                               and wp.get_cuda_device_count() > 0))
    if not gpu:
        out = p.copy()
        for _ in range(iterations):
            out = _cpu_step(out, masses, radius, separation, cohesion, cohesion_range)
        return out
    if wp is None or wp.get_cuda_device_count() == 0:
        raise RuntimeError("Warp CUDA requested but not installed/available")
    wp.init()
    search_radius = float(2.0 * np.max(radius) * cohesion_range)
    x = wp.array(p, dtype=wp.vec3, device="cuda:0")
    inv = wp.array(masses, dtype=wp.float32, device="cuda:0")
    rr = wp.array(radius, dtype=wp.float32, device="cuda:0")
    y = wp.zeros(len(p), dtype=wp.vec3, device="cuda:0")
    side = max(8, int(np.ceil(len(p) ** (1.0 / 3.0))))
    grid = wp.HashGrid(dim_x=side, dim_y=side, dim_z=side, device="cuda:0")
    for _ in range(iterations):
        grid.build(x, search_radius)
        wp.launch(_contact_jacobi, dim=len(p),
                  inputs=[grid.id, x, inv, rr, separation, cohesion,
                          cohesion_range, search_radius], outputs=[y], device="cuda:0")
        x, y = y, x
    return x.numpy()


def self_test(device):
    p = np.array([[0, 0, 0], [0.6, 0, 0]], dtype=np.float32)
    inv = np.ones(2, dtype=np.float32)
    radii = np.full(2, 0.5, dtype=np.float32)
    out = project(p, inv, radii, device=device, iterations=3)
    assert out[0, 0] < 0 and out[1, 0] > 0.6, out
    assert np.allclose(p[:, 0], [0.0, 0.6]), "input mutated"
    wet = project([[0, 0, 0], [1.2, 0, 0]], inv, radii,
                  device=device, cohesion=0.2, iterations=3)
    assert wet[0, 0] > 0 and wet[1, 0] < 1.2, wet
    print("Warp granular probe passed:", device)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.device)
    else:
        parser.print_help()
