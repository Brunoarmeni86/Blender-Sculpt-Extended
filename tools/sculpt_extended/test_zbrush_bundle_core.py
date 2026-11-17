"""Run: python -m unittest discover -s tools/sculpt_extended -p 'test_*.py'"""

import json
import tempfile
import unittest
from pathlib import Path

from zbrush_bundle_core import BundleValidationError, LayerSnapshot, MeshSnapshot, export_bundle, topology_hash


BASE = MeshSnapshot(
    vertices=((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)),
    faces=((0, 1, 2, 3),),
)
MOVE = MeshSnapshot(
    vertices=((0.0, 0.0, 0.0), (1.0, 0.0, 0.2), (1.0, 1.0, 0.2), (0.0, 1.0, 0.0)),
    faces=BASE.faces,
)
SCAR = MeshSnapshot(
    vertices=((0.0, 0.0, 0.1), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.1)),
    faces=BASE.faces,
)


class BundleTests(unittest.TestCase):
    def test_exports_same_indices_and_metadata(self):
        layers = [LayerSnapshot("Move Jaw", MOVE, 0.5, True), LayerSnapshot("Scar/Detail", SCAR, 1.0, False)]
        preview = MeshSnapshot(
            vertices=((0., 0., 0.), (1., 0., 0.1), (1., 1., 0.1), (0., 1., 0.)),
            faces=BASE.faces,
        )
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "export"
            report = export_bundle(dest, BASE, layers, composed=preview)
            m = json.loads((dest / "manifest.json").read_text())
            self.assertEqual(m["ordered_topology_sha256"], topology_hash(BASE))
            self.assertEqual([x["original_strength"] for x in m["layers"]], [0.5, 1.0])
            self.assertEqual([x["original_visibility"] for x in m["layers"]], [True, False])
            self.assertTrue((dest / "layer_002_Scar_Detail.obj").is_file())
            self.assertLess(report["composition_error"]["max_absolute"], 1e-12)
            for filename in ["baseline_high.obj", *(x["file"] for x in m["layers"]), "preview_composed.obj"]:
                text = (dest / filename).read_text()
                self.assertEqual(sum(line.startswith("v ") for line in text.splitlines()), 4)
                self.assertEqual([line for line in text.splitlines() if line.startswith("f ")], ["f 1 2 3 4"])

    def test_rejects_order_change(self):
        changed = MeshSnapshot(vertices=MOVE.vertices, faces=((0, 2, 1, 3),))
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(BundleValidationError, "topology mismatch"):
                export_bundle(Path(tmp) / "a", BASE, [LayerSnapshot("Mismatch", changed)])

    def test_rejects_invalid_values(self):
        broken = MeshSnapshot(vertices=((float("nan"), 0., 0.), *BASE.vertices[1:]), faces=BASE.faces)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(BundleValidationError, "invalid vertex"):
                export_bundle(Path(tmp) / "a", BASE, [LayerSnapshot("Bad", broken)])

    def test_rejects_nonadditive_composition_in_strict_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(BundleValidationError, "weighted sum"):
                export_bundle(Path(tmp) / "a", BASE, [LayerSnapshot("Move", MOVE)], composed=BASE)
            self.assertFalse((Path(tmp) / "a").exists())

    def test_warns_on_nonadditivity_and_no_preview(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = export_bundle(Path(tmp) / "a", BASE, [LayerSnapshot("Move", MOVE)], composed=BASE, strict_composition=False)
            self.assertTrue(report["warnings"])
            no_preview = export_bundle(Path(tmp) / "b", BASE, [LayerSnapshot("Move", MOVE)])
            self.assertIsNone(no_preview["composition_error"])

    def test_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "a"
            export_bundle(dest, BASE, [LayerSnapshot("Move", MOVE)])
            with self.assertRaises(FileExistsError):
                export_bundle(dest, BASE, [LayerSnapshot("Move", MOVE)])


if __name__ == "__main__":
    unittest.main()
