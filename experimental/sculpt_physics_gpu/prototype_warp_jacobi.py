"""Deterministic Warp/CUDA deformation prototype for Sculpt XPBD Grab.

This intentionally does not mutate Blender data.  It validates the GPU
algorithm before the native editor adapter is allowed to select CUDA.
"""

from __future__ import annotations

import argparse
import math
import time

import numpy as np
import warp as wp


@wp.kernel
def jacobi_shape_step(
        positions: wp.array(dtype=wp.vec3),
        rest_positions: wp.array(dtype=wp.vec3),
        inverse_mass: wp.array(dtype=float),
        offsets: wp.array(dtype=int),
        neighbors: wp.array(dtype=int),
        targets: wp.array(dtype=wp.vec3),
        target_weights: wp.array(dtype=float),
        active_indices: wp.array(dtype=int),
        material_stiffness: float,
        output: wp.array(dtype=wp.vec3)):
    i = active_indices[wp.tid()]
    p = positions[i]
    if inverse_mass[i] <= 0.0:
        output[i] = p
        return

    begin = offsets[i]
    end = offsets[i + 1]
    shape_sum = wp.vec3(0.0, 0.0, 0.0)
    count = int(0)
    for cursor in range(begin, end):
        j = neighbors[cursor]
        # Each neighbour predicts i while preserving the rest edge vector.
        shape_sum += positions[j] + (rest_positions[i] - rest_positions[j])
        count += 1

    material = p
    if count > 0:
        material = wp.lerp(p, shape_sum / float(count), material_stiffness)

    # Target weights are bounded before upload.  Jacobi reads only the prior
    # buffer, making the result independent of edge traversal order.
    output[i] = wp.lerp(material, targets[i], target_weights[i])


def grid_topology(side: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    count = side * side
    rest = np.zeros((count, 3), dtype=np.float32)
    adjacency: list[list[int]] = [[] for _ in range(count)]
    for y in range(side):
        for x in range(side):
            i = y * side + x
            rest[i] = (float(x), float(y), 0.0)
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < side and 0 <= ny < side:
                    adjacency[i].append(ny * side + nx)
    offsets = np.zeros(count + 1, dtype=np.int32)
    flat: list[int] = []
    for i, adjacent in enumerate(adjacency):
        flat.extend(adjacent)
        offsets[i + 1] = len(flat)
    return rest, offsets, np.asarray(flat, dtype=np.int32)


def solve(side: int, iterations: int, device: str) -> tuple[np.ndarray, float]:
    rest, offsets, neighbors = grid_topology(side)
    count = len(rest)
    center = (side - 1) * 0.5
    targets = rest.copy()
    weights = np.zeros(count, dtype=np.float32)
    inverse_mass = np.ones(count, dtype=np.float32)
    radius = max(4.0, side * 0.22)
    for y in range(side):
        for x in range(side):
            i = y * side + x
            distance = math.hypot(x - center, y - center)
            factor = max(0.0, 1.0 - distance / radius)
            factor = factor * factor * (3.0 - 2.0 * factor)
            targets[i, 2] += 8.0 * factor
            weights[i] = 0.72 * factor
            if x == 0 or y == 0 or x == side - 1 or y == side - 1:
                inverse_mass[i] = 0.0

    position_a = wp.array(rest, dtype=wp.vec3, device=device)
    position_b = wp.empty_like(position_a)
    rest_gpu = wp.array(rest, dtype=wp.vec3, device=device)
    inverse_mass_gpu = wp.array(inverse_mass, dtype=float, device=device)
    offsets_gpu = wp.array(offsets, dtype=int, device=device)
    neighbors_gpu = wp.array(neighbors, dtype=int, device=device)
    targets_gpu = wp.array(targets, dtype=wp.vec3, device=device)
    weights_gpu = wp.array(weights, dtype=float, device=device)
    active_gpu = wp.array(np.flatnonzero(weights > 0.0).astype(np.int32), dtype=int, device=device)

    wp.synchronize_device(device)
    start = time.perf_counter()
    for _ in range(iterations):
        wp.launch(
            jacobi_shape_step,
            dim=len(active_gpu),
            inputs=(position_a, rest_gpu, inverse_mass_gpu, offsets_gpu,
                    neighbors_gpu, targets_gpu, weights_gpu, active_gpu, 0.82),
            outputs=(position_b,),
            device=device)
        position_a, position_b = position_b, position_a
    wp.synchronize_device(device)
    elapsed = time.perf_counter() - start
    return position_a.numpy(), elapsed


def validate(side: int, iterations: int, device: str) -> None:
    result, elapsed = solve(side, iterations, device)
    z = result[:, 2].reshape((side, side))
    # High-frequency checkerboard energy detects the triangulation-like ripple
    # that made the raw edge XPBD integration unusable.
    checker = np.fromfunction(lambda y, x: 1.0 - 2.0 * ((x + y) % 2), z.shape)
    checker_energy = abs(float(np.mean(z * checker)))
    peak = float(np.max(z))
    assert np.isfinite(result).all()
    assert peak > 0.5
    assert checker_energy < 1.0e-3, checker_energy
    print(f"GPU_JACOBI_OK device={device} vertices={side * side} "
          f"iterations={iterations} elapsed_ms={elapsed * 1000.0:.3f} "
          f"peak={peak:.6f} checker_energy={checker_energy:.9f}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--side", type=int, default=128)
    parser.add_argument("--iterations", type=int, default=24)
    parser.add_argument("--device", default="cuda:0")
    args = parser.parse_args()
    wp.init()
    validate(args.side, args.iterations, args.device)


if __name__ == "__main__":
    main()
