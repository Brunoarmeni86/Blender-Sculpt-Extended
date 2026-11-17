# SPDX-License-Identifier: GPL-2.0-or-later
"""ZBrush Sculpt Layer OBJ bundle builder (data-only V01).

This module NEVER changes Blender state. A later native Multires adapter must
supply independent, identical-index evaluated snapshots. No bpy dependency.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import tempfile
from typing import Sequence


class ExportError(ValueError):
    """Invalid or unsafe ZBrush interchange input."""


@dataclass(frozen=True)
class MeshSnapshot:
    # Ordered mesh vertices (object space), preserving evaluated point identity.
    vertices: Sequence[Sequence[float]]
    # Ordered face corners; 0-based indices; quads/ngons preserved as supplied.
    faces: Sequence[Sequence[int]]


@dataclass(frozen=True)
class GeometryLayer:
    # This snapshot MUST contain only this layer at strength 1, all others muted.
    name: str
    strength: float
    visible: bool
    isolated: MeshSnapshot


def topology_hash(mesh: MeshSnapshot) -> str:
    """Hash vertex count and ordered face corner indices, not coordinates."""
    h = hashlib.sha256()
    h.update(f"vertices={len(mesh.vertices)};faces={len(mesh.faces)}\n".encode("ascii"))
    for face in mesh.faces:
        h.update((",".join(str(i) for i in face) + ";").encode("ascii"))
    return h.hexdigest()


def _validate(mesh: MeshSnapshot, label: str) -> None:
    n = len(mesh.vertices)
    if n < 3 or not mesh.faces:
        raise ExportError(f"{label}: empty/incomplete mesh")
    for k, v in enumerate(mesh.vertices):
        if len(v) != 3 or any(not math.isfinite(c) for c in v):
            raise ExportError(f"{label}: non-finite or malformed vertex {k}")
    for k, f in enumerate(mesh.faces):
        if len(f) < 3 or len(set(f)) != len(f) or any(
            not isinstance(ix, int) or isinstance(ix, bool) or ix < 0 or ix >= n
            for ix in f
        ):
            raise ExportError(f"{label}: invalid ordered face {k}")


def _bbox_diagonal(mesh: MeshSnapshot) -> float:
    lo = [min(v[a] for v in mesh.vertices) for a in range(3)]
    hi = [max(v[a] for v in mesh.vertices) for a in range(3)]
    return math.sqrt(sum((hi[i] - lo[i]) ** 2 for i in range(3)))


def _write_obj(path: Path, mesh: MeshSnapshot) -> None:
    # Deliberately do not use Blender's OBJ exporter: we must preserve indices.
    with path.open("w", encoding="ascii", newline="\n") as f:
        f.write("# Sculpt Extended ZBrush Bridge V01; ordered object-space vertices\n")
        for x, y, z in mesh.vertices:
            f.write(f"v {x:.17g} {y:.17g} {z:.17g}\n")
        for face in mesh.faces:
            f.write("f " + " ".join(str(i + 1) for i in face) + "\n")


def build_bundle(
    destination: str | Path,
    baseline: MeshSnapshot,
    layers: Sequence[GeometryLayer],
    composed: MeshSnapshot | None = None,
    *,
    relative_tolerance: float = 1.0e-4,
) -> dict:
    """Write one OBJ per isolated geometry layer + manifest, with strict checks.

    The folder must not already exist. All inputs are validated BEFORE any file
    is published. If composed is provided, reject non-additive layer results.
    This guards correctness but does not verify ZBrush import vertex remapping.
    """
    if not math.isfinite(relative_tolerance) or relative_tolerance < 0:
        raise ExportError("invalid relative_tolerance")
    _validate(baseline, "baseline")
    fp = topology_hash(baseline)
    if not layers:
        raise ExportError("no geometry layers supplied")
    for i, item in enumerate(layers):
        if not item.name or not math.isfinite(item.strength):
            raise ExportError(f"layer {i}: name/strength invalid")
        if not isinstance(item.visible, bool):
            raise ExportError(f"layer {i}: visible must be boolean")
        _validate(item.isolated, f"layer {i}")
        if topology_hash(item.isolated) != fp:
            raise ExportError(f"layer {i}: ordered topology mismatch")

    if composed is not None:
        _validate(composed, "composed")
        if topology_hash(composed) != fp:
            raise ExportError("composed: ordered topology mismatch")

    diag = _bbox_diagonal(baseline)
    scale = max(diag, 1.0e-12)
    maximum_error = 0.0
    rms_error = 0.0
    if composed is not None:
        errors2 = 0.0
        for vertex_index, bv in enumerate(baseline.vertices):
            point = [float(x) for x in bv]
            for layer in layers:
                if layer.visible and layer.strength != 0:
                    lv = layer.isolated.vertices[vertex_index]
                    for k in range(3):
                        point[k] += layer.strength * (lv[k] - bv[k])
            cv = composed.vertices[vertex_index]
            delta = math.sqrt(sum((point[k] - cv[k]) ** 2 for k in range(3)))
            maximum_error = max(maximum_error, delta)
            errors2 += delta ** 2
        rms_error = math.sqrt(errors2 / len(baseline.vertices))
        if maximum_error > relative_tolerance * scale:
            raise ExportError(
                f"non-additive result: max residual {maximum_error / scale:.3g} "
                f"of bbox diagonal exceeds {relative_tolerance:.3g}"
            )

    target = Path(destination)
    if target.exists() or target.is_symlink():
        raise ExportError(f"destination exists: {target}")
    parent = target.parent
    if not parent.is_dir():
        raise ExportError(f"parent does not exist: {parent}")
    staging = Path(tempfile.mkdtemp(prefix=".zbridge-staging-", dir=parent))
    manifest = {
        "schema": "sculpt-extended-zbrush-obj-v01",
        "geometry_space": "object-local; no axis conversion",
        "subdivision": "high-resolution mesh only; reconstruct lower levels in ZBrush as a separate validated step",
        "topology_sha256": fp,
        "vertex_count": len(baseline.vertices),
        "face_count": len(baseline.faces),
        "bbox_diagonal": diag,
        "relative_tolerance": relative_tolerance,
        "max_composition_error": maximum_error if composed is not None else None,
        "rms_composition_error": rms_error if composed is not None else None,
        "baseline": "baseline_high.obj",
        "composed_preview": "preview_composed.obj" if composed is not None else None,
        "layers": [],
        "zbrush_import_warning": "Each layer OBJ is an absolute baseline+single-layer mesh; hide other layers during REC import. ZBrush may reorder points; verify before importing layers.",
    }
    try:
        _write_obj(staging / "baseline_high.obj", baseline)
        for i, layer in enumerate(layers, 1):
            file_name = f"layer_{i:03d}.obj"
            _write_obj(staging / file_name, layer.isolated)
            manifest["layers"].append({
                "name": layer.name,
                "file": file_name,
                "strength": layer.strength,
                "visible": layer.visible,
                "index": i - 1,
            })
        if composed is not None:
            _write_obj(staging / "preview_composed.obj", composed)
        with (staging / "manifest.json").open("w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=2)
            f.write("\n")
        # Rename only after files are complete; no overwrite of existing target.
        if target.exists() or target.is_symlink():
            raise ExportError(f"destination appeared during export: {target}")
        os.rename(staging, target)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return manifest
