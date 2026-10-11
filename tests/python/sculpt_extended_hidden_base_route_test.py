"""Compile actual native-base routing fragments; this is not a Blender runtime test."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    kernel = (ROOT / "source/blender/blenkernel/intern/multires.cc").read_text()
    sculpt = (ROOT / "source/blender/editors/sculpt_paint/mesh/sculpt.cc").read_text()
    undo = (ROOT / "source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc").read_text()
    layers_source = (ROOT / "source/blender/blenkernel/intern/multires_sculpt_layers.cc").read_text()
    start = layers_source.index("bool base_has_visible_layers(")
    predicate = layers_source[start:layers_source.index("\n}\n", start) + 2]
    flush = kernel[kernel.index("bool multires_flush_sculpt_updates("):]
    start = flush.index("  if (!bke::multires_layers::base_has_visible_layers(*mmd))")
    branch = flush[start:flush.index("\n  else {", start)]
    start = sculpt.index("  /* With every layer hidden")
    admission = sculpt[start:sculpt.index("  if (!mmd->sculpt_layer_recording", start)]
    snapshot = undo[undo.index("static void store_sculpt_layer_disps("):]
    start = snapshot.index("  if (mmd.totlvl <= 0")
    snapshot = snapshot[start:snapshot.index("  const MultiresSculptLayer *layer", start)]
    cpp = r'''
#include <cassert>
#include <vector>
constexpr int MULTIRES_SCULPT_LAYER_VISIBLE = 1;
struct Layer { int flag; float strength; int delta; };
using MultiresSculptLayer = Layer;
struct MultiresModifierData { std::vector<Layer> sculpt_layers; int totlvl = 3; };
namespace bke::multires_layers {
PREDICATE
void weight_hot_delta_cache_invalidate() {}
}
struct Mesh { int base = 7; };
struct CCG { int position = 11; bool fail = false; };
bool multiresModifier_reshapeFromCCG(int, Mesh *mesh, CCG *ccg) {
  if (ccg->fail) return false;
  mesh->base = ccg->position;
  return true;
}
bool flush_base(MultiresModifierData *mmd, Mesh *mesh, CCG *ccg) {
BRANCH
  return mesh->base == ccg->position;
}
bool admitted(MultiresModifierData *mmd) {
ADMISSION
  return false;
}
void snapshot_layer(const MultiresModifierData &mmd, bool &captured) {
SNAPSHOT
  captured = true;
}
int main() {
  for (const auto &layers : {std::vector<Layer>{}, std::vector<Layer>{{0,1,42}},
       std::vector<Layer>{{0,1,42},{0,0,99}}, std::vector<Layer>{{1,0,42}},
       std::vector<Layer>{{0,1,42},{1,1,99}}}) {
    MultiresModifierData mmd{layers};
    const bool hidden = !bke::multires_layers::base_has_visible_layers(mmd);
    Mesh mesh; CCG ccg;
    assert(admitted(&mmd) == hidden);
    assert(flush_base(&mmd,&mesh,&ccg) == hidden);
    assert(mesh.base == (hidden ? 11 : 7));
    for (unsigned i=0; i<layers.size(); ++i) assert(mmd.sculpt_layers[i].delta == layers[i].delta);
    bool captured=false;
    snapshot_layer(mmd,captured);
    assert(captured == !hidden);
    ccg.fail=true; mesh.base=7;
    assert(!flush_base(&mmd,&mesh,&ccg));
    assert(mesh.base == 7);
  }
}
'''.replace("BRANCH", branch).replace("ADMISSION", admission).replace("SNAPSHOT", snapshot).replace("PREDICATE", predicate)
    with tempfile.TemporaryDirectory() as temp:
        source=Path(temp)/"routing.cc"
        exe=Path(temp)/"routing"
        source.write_text(cpp)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: 5 visibility cases, hidden payload preservation, reshape failure")


if __name__ == "__main__":
    main()
