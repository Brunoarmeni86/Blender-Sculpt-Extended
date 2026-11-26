"""Generate the native Sculpt Jacobi and Smooth PTX package for Warp 1.19.

Run with the pinned Warp source/binary tree on PYTHONPATH. A CUDA device is
required for the smoke launches. Export only modules containing the exact
entry symbols hardcoded in warp_cuda_backend.cc; never rename mismatched
entries or silently substitute another Warp version.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import sys
import tempfile

import numpy as np
import warp as wp

EXPECTED = {
    "blender_xpbd_jacobi.sm75.ptx": (
        "jacobi_shape_step_2135c77f_cuda_kernel_forward",
    ),
    "blender_smooth_a3.sm75.ptx": (
        "sculpt_smooth_laplacian_b88c9770_cuda_kernel_forward",
        "sculpt_smooth_scatter_4021f8a1_cuda_kernel_forward",
    ),
}


def entries(path):
    return set(re.findall(r"\.entry\s+([A-Za-z0-9_]+)\s*\(",
                          path.read_text(encoding="utf-8", errors="ignore")))


def find_modules(cache):
    found = {}
    observed = set()
    for path in sorted(cache.rglob("*.ptx"),
                       key=lambda p: p.stat().st_mtime, reverse=True):
        names = entries(path)
        observed.update(n for n in names if n.startswith(
            ("jacobi_shape_step_", "sculpt_smooth_laplacian_", "sculpt_smooth_scatter_")))
        for filename, required in EXPECTED.items():
            if filename not in found and set(required).issubset(names):
                found[filename] = path
    return found, observed


def smoke_jacobi(fast_math):
    import prototype_warp_jacobi as j
    wp.set_module_options({"fast_math": fast_math}, module=j)
    device = "cuda:0"
    positions = wp.array([[0., 0., 0.], [1., 0., 0.]], dtype=wp.vec3, device=device)
    rest = wp.clone(positions)
    targets = wp.array([[0., 0., 1.], [1., 0., 0.]], dtype=wp.vec3, device=device)
    mass = wp.array([1., 0.], dtype=float, device=device)
    offsets = wp.array([0, 1, 2], dtype=int, device=device)
    neighbors = wp.array([1, 0], dtype=int, device=device)
    active = wp.array([0, 1], dtype=int, device=device)
    weights = wp.array([.5, 0.], dtype=float, device=device)
    output = wp.zeros(2, dtype=wp.vec3, device=device)
    wp.launch(j.jacobi_shape_step, dim=2,
              inputs=[positions, rest, mass, offsets, neighbors, targets,
                      weights, active, .82], outputs=[output], device=device)
    wp.synchronize()
    np.testing.assert_allclose(output.numpy(), [[0., 0., .5], [1., 0., 0.]],
                               rtol=1e-5, atol=1e-6)


def smoke_smooth(fast_math):
    import prototype_warp_smooth as s
    wp.set_module_options({"fast_math": fast_math}, module=s)
    device = "cuda:0"
    positions = wp.array([[0., 0., 0.], [1., 0., 0.]], dtype=wp.vec3, device=device)
    offsets = wp.array([0, 1, 2], dtype=wp.int32, device=device)
    neighbors = wp.array([1, 0], dtype=wp.int32, device=device)
    active = wp.array([0], dtype=wp.int32, device=device)
    weights = wp.array([.25], dtype=wp.float32, device=device)
    normals = wp.array([[0., 0., 1.]], dtype=wp.vec3, device=device)
    output = wp.zeros(1, dtype=wp.vec3, device=device)
    wp.launch(s.sculpt_smooth_laplacian, dim=1,
              inputs=[positions, offsets, neighbors, active, weights, normals,
                      4, 1., 1.], outputs=[output], device=device)
    wp.launch(s.sculpt_smooth_scatter, dim=1,
              inputs=[active, output, positions], device=device)
    wp.synchronize()
    result = positions.numpy()
    assert np.isfinite(result).all() and 0. < result[0, 0] <= 1.
    np.testing.assert_allclose(result[1], [1., 0., 0.], atol=1e-6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    version = str(wp.__version__)
    print("Warp:", version, "from", wp.__file__)
    print("Python:", sys.executable)
    if version != "1.19.0":
        raise RuntimeError("The native backend is pinned to Warp 1.19.0")
    wp.config.cuda_output = "ptx"
    wp.config.ptx_target_arch = 75
    # Fresh cache prevents exporting a stale binary from another source/version.
    with tempfile.TemporaryDirectory(prefix="bse-warp-ptx-") as directory:
        wp.config.kernel_cache_dir = directory
        wp.init()
        if not wp.is_cuda_available():
            raise RuntimeError("Warp CUDA device unavailable")
        print("Device:", wp.get_device("cuda:0"))
        # Original Smooth prototype uses fast_math=True; test both options
        # only if the expected entry is missing.
        smoke_jacobi(False)
        smoke_smooth(True)
        cache = Path(wp.config.kernel_cache_dir)
        found, observed = find_modules(cache)
        if "blender_xpbd_jacobi.sm75.ptx" not in found:
            smoke_jacobi(True)
        if "blender_smooth_a3.sm75.ptx" not in found:
            smoke_smooth(False)
        found, observed = find_modules(cache)
        missing = set(EXPECTED) - set(found)
        if missing:
            print("Generated kernel entries:", *sorted(observed), sep="\n  ")
            raise RuntimeError("Native entry mismatch/missing PTX: " +
                               ", ".join(sorted(missing)) +
                               ". No files copied; inspect native symbols and generated PTX.")
        args.output.mkdir(parents=True, exist_ok=True)
        for filename, source in found.items():
            destination = args.output / filename
            shutil.copyfile(source, destination)
            assert set(EXPECTED[filename]).issubset(entries(destination))
            print("[OK]", destination.resolve())
        print("CUDA smoke fixtures passed; Blender runtime still requires testing.")


if __name__ == "__main__":
    main()
