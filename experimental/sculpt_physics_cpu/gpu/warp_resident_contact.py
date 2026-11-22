"""Persistent local granular contact backend for NVIDIA Warp.

Only a standalone GPU/CPU experiment, NOT native Blender or a GPU XPBD World.
Unlike warp_granular_probe.project(), the CUDA positions, inverse masses,
radii, HashGrid and ping-pong buffers remain alive across many brush dabs.

One-time full proxy upload -> GPU-resident solve -> optional *sparse* edits
and readback. Never read all particles implicitly after a solve.

Command-line tests:
    python gpu/test_warp_resident_contact.py --device cpu
    python gpu/test_warp_resident_contact.py --device cuda  # Warp+NVIDIA

Source reference: NVIDIA Warp example_dem.py (Apache-2.0). Kernels here
are original implementations using the public Warp API.
"""
from __future__ import annotations

from dataclasses import dataclass
import numpy as np

from warp_granular_probe import _cpu_step, wp

if wp is not None:
    from warp_granular_probe import _contact_jacobi

    @wp.kernel
    def _scatter_positions(
        dst: wp.array(dtype=wp.vec3),
        indices: wp.array(dtype=wp.int32),
        values: wp.array(dtype=wp.vec3),
    ):
        i = wp.tid()
        dst[indices[i]] = values[i]

    @wp.kernel
    def _gather_positions(
        src: wp.array(dtype=wp.vec3),
        indices: wp.array(dtype=wp.int32),
        result: wp.array(dtype=wp.vec3),
    ):
        i = wp.tid()
        result[i] = src[indices[i]]


@dataclass(frozen=True)
class TransferStats:
    initial_upload_bytes: int
    sparse_upload_bytes: int
    sparse_download_bytes: int
    full_download_bytes: int
    allocations: int


class ResidentContactSession:
    """One persistent, fixed-topology local contact island.

    The caller supplies *all contact neighbors* in the island, including
    active-halo particles. This solver does not preserve interactions outside
    that island, calculate mesh collisions, or write PBVH/CCG/Undo.

    Device='auto' uses CUDA only when Warp has a CUDA device. Device='cpu'
    runs a slow NumPy numerical oracle intended for small regression scenes;
    production CPU sculpting remains the existing C++ hash-grid solver.
    """

    def __init__(self, positions, inverse_masses, radii, *, device="auto"):
        if device not in ("auto", "cpu", "warp-cpu", "cuda"):
            raise ValueError("device must be 'auto', 'cpu', 'warp-cpu' or 'cuda'")
        points = np.asarray(positions, dtype=np.float32)
        mass = np.asarray(inverse_masses, dtype=np.float32)
        radius = np.asarray(radii, dtype=np.float32)
        if (points.ndim != 2 or points.shape[1] != 3 or
            mass.shape != (len(points),) or radius.shape != (len(points),) or
            not np.isfinite(points).all() or not np.isfinite(mass).all() or
            not np.isfinite(radius).all() or np.any(mass < 0.0) or
            np.any(radius <= 0.0) or len(points) > np.iinfo(np.int32).max):
            raise ValueError("invalid particle arrays")
        cuda_available = bool(wp is not None and wp.get_cuda_device_count() > 0)
        if device == "cuda" and not cuda_available:
            raise RuntimeError("Warp CUDA unavailable: no suitable NVIDIA device/runtime")
        if device == "warp-cpu" and wp is None:
            raise RuntimeError("Warp CPU runtime unavailable (install warp-lang)")
        self._cuda = device == "cuda" or (device == "auto" and cuda_available)
        self._warp = self._cuda or device == "warp-cpu"
        self._n = len(points)
        self._max_radius = float(np.max(radius)) if self._n else 0.0
        self._initial_upload = int(points.nbytes + mass.nbytes + radius.nbytes) if self._cuda else 0
        self._sparse_upload = 0
        self._sparse_download = 0
        self._full_download = 0
        self._allocations = 0
        self._device = "cuda:0" if self._cuda else "cpu"
        self._host_inv_mass = mass.copy()
        if self._warp and self._n:
            wp.init()
            self._x = wp.array(points, dtype=wp.vec3, device=self._device)
            self._y = wp.zeros(self._n, dtype=wp.vec3, device=self._device)
            self._inv = wp.array(mass, dtype=wp.float32, device=self._device)
            self._radii = wp.array(radius, dtype=wp.float32, device=self._device)
            side = max(8, int(np.ceil(self._n ** (1.0 / 3.0))))
            self._grid = wp.HashGrid(side, side, side, device=self._device)
            self._allocations += 5
        else:
            # The CPU oracle keeps its own independent arrays.
            self._cpu_positions = np.array(points, copy=True)
            self._cpu_masses = np.array(mass, copy=True)
            self._cpu_radii = np.array(radius, copy=True)

    @property
    def device(self):
        return "cuda" if self._cuda else ("warp-cpu" if self._warp else "cpu")

    @property
    def particle_count(self):
        return self._n

    @property
    def stats(self):
        return TransferStats(self._initial_upload, self._sparse_upload,
                             self._sparse_download, self._full_download,
                             self._allocations)

    def _indices(self, indices):
        ids = np.asarray(indices)
        if ids.ndim != 1:
            raise ValueError("particle indices must be a 1-D array")
        if len(ids) == 0:
            return np.empty(0, dtype=np.int32)
        if not np.issubdtype(ids.dtype, np.integer):
            raise ValueError("particle indices must contain integers")
        if np.any(ids < 0) or np.any(ids >= self._n):
            raise IndexError("particle index outside persistent contact island")
        if len(np.unique(ids)) != len(ids):
            raise ValueError("duplicate particle IDs in a sparse transfer")
        return np.asarray(ids, dtype=np.int32)

    def update_positions(self, indices, positions):
        """Stage an artist/proxy edit for selected local IDs only."""
        ids = self._indices(indices)
        pts = np.asarray(positions, dtype=np.float32)
        if pts.shape != (len(ids), 3) or not np.isfinite(pts).all():
            raise ValueError("invalid sparse positions")
        if not len(ids):
            return
        # Fixed exterior/halo particles must never be edited by the brush.
        # Native Blender owns the actual Undo scope and must validate it too.
        if np.any(self._host_inv_mass[ids] <= 0.0):
            raise ValueError("cannot edit pinned/zero-inverse-mass particles")
        if not self._warp:
            self._cpu_positions[ids] = pts
            return
        # Allocates only O(dirty_count), NOT O(proxy_count) per dab.
        # Future optimization: reusable pinned host / GPU scratch buffers.
        ids_gpu = wp.array(ids, dtype=wp.int32, device=self._device)
        pts_gpu = wp.array(pts, dtype=wp.vec3, device=self._device)
        wp.launch(_scatter_positions, dim=len(ids), inputs=[self._x, ids_gpu, pts_gpu],
                  device=self._device)
        if self._cuda:
            self._sparse_upload += ids.nbytes + pts.nbytes
        self._allocations += 2

    def step(self, *, iterations=4, separation=0.85,
             cohesion=0.0, cohesion_range=1.25):
        """Solve on resident arrays; NO implicit host readback."""
        if (not isinstance(iterations, int) or not 1 <= iterations <= 64 or
            not np.isfinite(separation) or not 0 <= separation <= 1 or
            not np.isfinite(cohesion) or not 0 <= cohesion <= 1 or
            not np.isfinite(cohesion_range) or not 1 <= cohesion_range <= 8):
            raise ValueError("invalid contact parameters")
        if self._n <= 1:
            return
        if not self._warp:
            for _ in range(iterations):
                self._cpu_positions = _cpu_step(
                    self._cpu_positions, self._cpu_masses, self._cpu_radii,
                    separation, cohesion, cohesion_range)
            if not np.isfinite(self._cpu_positions).all():
                raise FloatingPointError("CPU oracle contact divergence")
            return
        search = float(2.0 * self._max_radius * cohesion_range)
        if not np.isfinite(search) or search <= 0:
            raise ValueError("invalid hash-grid neighborhood radius")
        for _ in range(iterations):
            # HashGrid is rebuilt because particle positions change each
            # iteration, but the grid object and memory remain persistent.
            self._grid.build(self._x, search)
            wp.launch(
                _contact_jacobi, dim=self._n,
                inputs=[self._grid.id, self._x, self._inv, self._radii,
                        float(separation), float(cohesion),
                        float(cohesion_range), search],
                outputs=[self._y], device=self._device)
            self._x, self._y = self._y, self._x

    def read_positions(self, indices=None):
        """Explicit readback: subset only, unless indices=None is requested."""
        if indices is None:
            if not self._warp:
                return self._cpu_positions.copy()
            if not self._n:
                return np.empty((0, 3), dtype=np.float32)
            result = self._x.numpy()
            self._full_download += result.nbytes
        else:
            ids = self._indices(indices)
            if not len(ids):
                return np.empty((0, 3), dtype=np.float32)
            if not self._warp:
                return self._cpu_positions[ids].copy()
            ids_gpu = wp.array(ids, dtype=wp.int32, device=self._device)
            result_gpu = wp.empty(len(ids), dtype=wp.vec3, device=self._device)
            wp.launch(_gather_positions, dim=len(ids),
                      inputs=[self._x, ids_gpu], outputs=[result_gpu],
                      device=self._device)
            result = result_gpu.numpy()
            if self._cuda:
                self._sparse_upload += ids.nbytes
                self._sparse_download += result.nbytes
            self._allocations += 2
        if not np.isfinite(result).all():
            raise FloatingPointError("non-finite GPU contact output: refuse writeback")
        return np.array(result, dtype=np.float32, copy=True)
