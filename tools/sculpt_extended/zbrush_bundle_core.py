"""Pure-Python, non-destructive OBJ bundle writer for ZBrush 3D Layer transfer.

This module deliberately knows nothing about bpy or native Sculpt Extended layer
storage. A future adapter must snapshot committed, evaluated Multires geometry
without changing the original object's state. No ZBrush API automation is implied.

The exported OBJ variants all use *identical point and face indices*, so a
ZBrush operator can record each isolated variant as an independent 3D Layer.
"""

from __future__ import annotations

from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
from typing import Sequence
import json
import math
import os
import re
import shutil
import struct
import tempfile


@dataclass(frozen=True)
class MeshSnapshot:
    """0-based vertices and ordered polygon corner lists, in object space."""

    vertices: tuple[tuple[float, float, float], ...]
    faces: tuple[tuple[int, ...], ...]


@dataclass(frozen=True)
class LayerSnapshot:
    """The evaluated mesh with *only this layer* enabled at strength 1.0."""

    name: str
    mesh: MeshSnapshot
    strength: float = 1.0
    visible: bool = True


class BundleValidationError(ValueError):
    """Input cannot be transferred as an index-preserving ZBrush layer bundle."""


def _validate_mesh(mesh: MeshSnapshot, label: str) -> None:
    n = len(mesh.vertices)
    if n == 0 or not mesh.faces:
        raise BundleValidationError(f"{label}: empty vertices or faces")
    for i, point in enumerate(mesh.vertices):
        if len(point) != 3 or not all(math.isfinite(c) for c in point):
            raise BundleValidationError(f"{label}: invalid vertex {i}")
    for i, face in enumerate(mesh.faces):
        if len(face) < 3 or len(set(face)) != len(face):
            raise BundleValidationError(f"{label}: invalid face {i}")
        if any(not isinstance(v, int) or v < 0 or v >= n for v in face):
            raise BundleValidationError(f"{label}: out-of-range face index in face {i}")


def topology_hash(mesh: MeshSnapshot) -> str:
    """Hash vertex count, ordered face count and each ordered polygon index."""
    digest = sha256()
    digest.update(struct.pack("<QQ", len(mesh.vertices), len(mesh.faces)))
    for face in mesh.faces:
        digest.update(struct.pack("<Q", len(face)))
        for index in face:
            digest.update(struct.pack("<Q", index))
    return digest.hexdigest()


def _bounds_diagonal(vertices: Sequence[tuple[float, float, float]]) -> float:
    lo = [min(v[axis] for v in vertices) for axis in range(3)]
    hi = [max(v[axis] for v in vertices) for axis in range(3)]
    return math.sqrt(sum((hi[i] - lo[i]) ** 2 for i in range(3)))


def _safe_name(name: str) -> str:
    stem = re.sub(r"[^A-Za-z0-9_-]+", "_", name.strip()).strip("_-")
    return (stem[:52] or "unnamed")


def _write_obj(path: Path, mesh: MeshSnapshot, title: str) -> None:
    with path.open("w", encoding="ascii", newline="\n") as handle:
        handle.write(f"# Sculpt Extended ZBrush bridge: {title.encode('ascii', 'replace').decode('ascii')}\n")
        for x, y, z in mesh.vertices:
            handle.write(f"v {x:.17g} {y:.17g} {z:.17g}\n")
        for face in mesh.faces:
            handle.write("f " + " ".join(str(index + 1) for index in face) + "\n")


def _composition_error(
    baseline: MeshSnapshot,
    layers: Sequence[LayerSnapshot],
    composed: MeshSnapshot,
) -> dict[str, float]:
    largest_sq = 0.0
    sum_sq = 0.0
    for i, p in enumerate(baseline.vertices):
        predicted = list(p)
        for layer in layers:
            if layer.visible and layer.strength:
                for axis in range(3):
                    predicted[axis] += layer.strength * (layer.mesh.vertices[i][axis] - p[axis])
        sq = sum((predicted[axis] - composed.vertices[i][axis]) ** 2 for axis in range(3))
        largest_sq = max(largest_sq, sq)
        sum_sq += sq
    scale = max(_bounds_diagonal(baseline.vertices), 1e-12)
    return {
        "max_absolute": math.sqrt(largest_sq),
        "rms_absolute": math.sqrt(sum_sq / len(baseline.vertices)),
        "max_relative_to_baseline_diagonal": math.sqrt(largest_sq) / scale,
        "rms_relative_to_baseline_diagonal": math.sqrt(sum_sq / len(baseline.vertices)) / scale,
    }


def export_bundle(
    destination: str | Path,
    baseline: MeshSnapshot,
    layers: Sequence[LayerSnapshot],
    *,
    composed: MeshSnapshot | None = None,
    relative_error_limit: float = 1e-4,
    strict_composition: bool = True,
) -> dict:
    """Create a new, index-consistent OBJ bundle; never overwrite an existing folder.

    ``baseline`` must be the evaluated highest-level mesh with *all* sculpt
    layers muted. ``layer.mesh`` must be evaluated with only that layer visible
    at strength 1 (not the delta mesh). ``composed`` is the actually evaluated
    original visible/strength-weighted sculpt for quality comparison.

    The caller owns Blender state management, level sampling, and the commit
    boundary. This function has no access to and cannot mutate source meshes.
    """
    _validate_mesh(baseline, "baseline")
    if not layers:
        raise BundleValidationError("At least one layer is required")
    if not math.isfinite(relative_error_limit) or relative_error_limit < 0:
        raise BundleValidationError("relative_error_limit must be finite and >= 0")
    fingerprint = topology_hash(baseline)
    for i, layer in enumerate(layers):
        if not isinstance(layer.name, str) or not layer.name.strip():
            raise BundleValidationError(f"layer {i}: empty name")
        if not isinstance(layer.visible, bool):
            raise BundleValidationError(f"layer {i}: visibility must be boolean")
        if not math.isfinite(layer.strength):
            raise BundleValidationError(f"layer {i}: non-finite strength")
        _validate_mesh(layer.mesh, f"layer {i}")
        if topology_hash(layer.mesh) != fingerprint:
            raise BundleValidationError(f"layer {i}: point/face count or ordered topology mismatch")
    if composed is not None:
        _validate_mesh(composed, "composed")
        if topology_hash(composed) != fingerprint:
            raise BundleValidationError("composed: point/face count or ordered topology mismatch")

    error = _composition_error(baseline, layers, composed) if composed is not None else None
    warnings: list[str] = []
    if error is None:
        warnings.append("No composed reference provided: layer additivity was not checked")
    elif error["max_relative_to_baseline_diagonal"] > relative_error_limit:
        msg = "Composed geometry is not reproduced by the weighted sum of isolated layers"
        if strict_composition:
            raise BundleValidationError(msg)
        warnings.append(msg)
    if any(len(f) != 4 for f in baseline.faces):
        warnings.append("Non-quad faces: ZBrush Reconstruct Subdiv may not recover subdivision levels")

    output = Path(destination)
    if output.exists():
        raise FileExistsError(f"Refusing to overwrite existing bundle: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temp_dir = Path(tempfile.mkdtemp(prefix=".zbrush_bridge_", dir=output.parent))
    try:
        _write_obj(temp_dir / "baseline_high.obj", baseline, "baseline")
        manifest_layers = []
        for i, layer in enumerate(layers, 1):
            filename = f"layer_{i:03d}_{_safe_name(layer.name)}.obj"
            _write_obj(temp_dir / filename, layer.mesh, layer.name)
            manifest_layers.append({
                "index": i,
                "name": layer.name,
                "file": filename,
                "original_strength": layer.strength,
                "original_visibility": layer.visible,
                "snapshot_strength": 1.0,
            })
        if composed is not None:
            _write_obj(temp_dir / "preview_composed.obj", composed, "composed reference")
        manifest = {
            "format": "sculpt-extended-zbrush-obj-layers",
            "version": 1,
            "coordinate_space": "object_local_no_axis_conversion",
            "high_resolution_vertices": len(baseline.vertices),
            "high_resolution_faces": len(baseline.faces),
            "ordered_topology_sha256": fingerprint,
            "base_file": "baseline_high.obj",
            "preview_file": "preview_composed.obj" if composed else None,
            "layers": manifest_layers,
            "zbrush_instruction": "Import base, create 3D Layer at highest subdivision in REC, then import one isolated OBJ with all other layers disabled. Validate point order.",
            "warning": "No native ZBrush subdivisions, layer files, color or GoZ automation are generated",
        }
        report = {
            "format_version": 1,
            "ordered_topology_sha256": fingerprint,
            "composition_error": error,
            "relative_error_limit": relative_error_limit,
            "warnings": warnings,
        }
        for name, payload in (("manifest.json", manifest), ("report.json", report)):
            (temp_dir / name).write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        os.rename(temp_dir, output)
    except Exception:
        shutil.rmtree(temp_dir, ignore_errors=True)
        raise
    return report
