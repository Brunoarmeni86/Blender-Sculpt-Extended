# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline tests: python -m unittest discover -s tools/zbrush_bridge -p 'test_*.py'."""
import json
from pathlib import Path
import tempfile
import unittest

from obj_bundle import ExportError, GeometryLayer, MeshSnapshot, build_bundle


class ObjBundleTests(unittest.TestCase):
    def setUp(self):
        self.quad = ((0, 1, 2, 3),)
        self.base = MeshSnapshot(
            ((0., 0., 0.), (1., 0., 0.), (1., 1., 0.), (0., 1., 0.)),
            self.quad,
        )
        self.a = MeshSnapshot(
            ((0., 0., 0.), (1., 0., 0.5), (1., 1., 0.), (0., 1., 0.)),
            self.quad,
        )
        self.b = MeshSnapshot(
            ((0., 0., 0.), (1., 0., 0.), (1., 1., 0.2), (0., 1., 0.)),
            self.quad,
        )
        self.layers = (
            GeometryLayer("Fold / 1", 0.5, True, self.a),
            GeometryLayer("Hidden detail", 1.0, False, self.b),
        )
        self.composed = MeshSnapshot(
            ((0., 0., 0.), (1., 0., 0.25), (1., 1., 0.), (0., 1., 0.)),
            self.quad,
        )

    def test_bundle_and_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder) / "export"
            manifest = build_bundle(dest, self.base, self.layers, self.composed)
            self.assertEqual(manifest["vertex_count"], 4)
            self.assertEqual(len(manifest["layers"]), 2)
            self.assertEqual(manifest["layers"][0]["strength"], 0.5)
            self.assertFalse(manifest["layers"][1]["visible"])
            self.assertEqual(manifest["max_composition_error"], 0)
            self.assertEqual(json.loads((dest / "manifest.json").read_text())["layers"][0]["name"], "Fold / 1")
            a = (dest / "layer_001.obj").read_text()
            self.assertIn("f 1 2 3 4", a)
            self.assertIn("v 1 0 0.5", a)
            self.assertTrue((dest / "preview_composed.obj").exists())

    def test_reject_changed_topology_before_writing(self):
        mismatched = MeshSnapshot(self.a.vertices, ((0, 2, 1, 3),))
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder) / "export"
            with self.assertRaisesRegex(ExportError, "topology mismatch"):
                build_bundle(dest, self.base, [GeometryLayer("x", 1., True, mismatched)])
            self.assertFalse(dest.exists())

    def test_reject_non_additive_preview_before_writing(self):
        wrong = MeshSnapshot(self.a.vertices, self.quad)
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder) / "export"
            with self.assertRaisesRegex(ExportError, "non-additive"):
                build_bundle(dest, self.base, self.layers, wrong)
            self.assertFalse(dest.exists())

    def test_reject_nonfinite(self):
        bad = MeshSnapshot(((float("nan"), 0, 0), *self.base.vertices[1:]), self.quad)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ExportError, "non-finite"):
                build_bundle(Path(folder) / "export", bad, self.layers)

    def test_refuse_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder) / "export"
            build_bundle(dest, self.base, self.layers)
            with self.assertRaisesRegex(ExportError, "destination exists"):
                build_bundle(dest, self.base, self.layers)


if __name__ == "__main__":
    unittest.main()
