"""Dedicated compact-region Warp kernels for Sculpt Smooth A2.

The topology and full position buffer remain resident. Only influenced vertex
indices and weights are uploaded per dab, and only compact results are read
back. Neighbours outside the active set form a read-only continuity collar.
"""

import warp as wp


@wp.kernel
def sculpt_smooth_laplacian(
    positions: wp.array(dtype=wp.vec3),
    offsets: wp.array(dtype=wp.int32),
    neighbors: wp.array(dtype=wp.int32),
    active: wp.array(dtype=wp.int32),
    weights: wp.array(dtype=wp.float32),
    active_normals: wp.array(dtype=wp.vec3),
    mode: wp.int32,
    pass_scale: wp.float32,
    reference_edge: wp.float32,
    output: wp.array(dtype=wp.vec3),
):
    i = wp.tid()
    vertex = active[i]
    p = positions[vertex]
    start = offsets[vertex]
    end = offsets[vertex + 1]
    average = wp.vec3(0.0, 0.0, 0.0)
    far_average = wp.vec3(0.0, 0.0, 0.0)
    count = end - start
    edge_sum = float(0.0)
    far_count = int(0)
    for edge in range(start, end):
        neighbor_index = neighbors[edge]
        neighbor = positions[neighbor_index]
        average = average + neighbor
        edge_sum = edge_sum + wp.length(neighbor - p)
        if mode == 4:
            far_start = offsets[neighbor_index]
            far_end = offsets[neighbor_index + 1]
            for far_edge in range(far_start, far_end):
                far_index = neighbors[far_edge]
                if far_index != vertex:
                    far_average = far_average + positions[far_index]
                    far_count = far_count + 1
    if count > 0:
        average = average / float(count)
        # Valence normalization keeps extraordinary vertices from receiving a
        # disproportionate impulse while preserving the familiar Laplacian.
        valence_scale = wp.sqrt(6.0 / wp.max(float(count), 1.0))
        local_edge = edge_sum / float(count)
        density_scale = wp.clamp(reference_edge / wp.max(local_edge, 1.0e-12), 0.5, 2.0)
        factor = wp.clamp(weights[i] * valence_scale * density_scale, 0.0, 1.0)
        delta = average - p
        if mode == 4 and far_count > 0:
            # A two-ring umbrella gives dense meshes a geometric support closer
            # to the same brush on a coarser surface. Repeated scale-adaptive
            # passes then approximate a local coarse proxy without building or
            # transferring a second mesh.
            far_average = far_average / float(far_count)
            delta = (average * 0.25 + far_average * 0.75) - p
        normal = wp.normalize(active_normals[i])
        normal_delta = normal * wp.dot(delta, normal)
        if mode == 2:  # Volume Preserve: strongly attenuate shrinkage normal to the surface.
            delta = delta - normal_delta * 0.9
        elif mode == 3:  # Relax: mainly tangential, with enough normal response to remove noise.
            delta = delta - normal_delta * 0.65
        output[i] = p + delta * factor * pass_scale
    else:
        output[i] = p


@wp.kernel
def sculpt_smooth_scatter(
    active: wp.array(dtype=wp.int32),
    compact: wp.array(dtype=wp.vec3),
    positions: wp.array(dtype=wp.vec3),
):
    i = wp.tid()
    positions[active[i]] = compact[i]


if __name__ == "__main__":
    wp.config.cuda_output = "ptx"
    wp.config.ptx_target_arch = 75
    wp.init()
    wp.set_module_options({"fast_math": True})
    # Launch once so Warp materializes the module in its kernel cache.
    device = "cuda:0"
    positions = wp.zeros(2, dtype=wp.vec3, device=device)
    offsets = wp.array([0, 1, 2], dtype=wp.int32, device=device)
    neighbors = wp.array([1, 0], dtype=wp.int32, device=device)
    active = wp.array([0], dtype=wp.int32, device=device)
    weights = wp.array([0.5], dtype=wp.float32, device=device)
    normals = wp.array([wp.vec3(0.0, 0.0, 1.0)], dtype=wp.vec3, device=device)
    output = wp.zeros(1, dtype=wp.vec3, device=device)
    wp.launch(sculpt_smooth_laplacian, dim=1,
              inputs=[positions, offsets, neighbors, active, weights, normals, 0, 1.0, 1.0],
              outputs=[output], device=device)
    wp.launch(sculpt_smooth_scatter, dim=1,
              inputs=[active, output, positions], device=device)
    wp.synchronize()
