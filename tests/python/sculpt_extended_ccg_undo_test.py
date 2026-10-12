"""Check actual CCG policy guards and simulate single-authority stroke undo.

Uses the existing synchronization model; no native Blender geometry is executed.
"""
from pathlib import Path
import subprocess
import tempfile
from fractions import Fraction as F
from sculpt_extended_undo_consistency_test import Model, Undo

ROOT = Path(__file__).resolve().parents[2]


def guards():
    source=(ROOT/"source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc").read_text()
    start=source.index("static bool sculpt_layer_undo_ccg_enabled()")
    helper=source[start:source.index("\n}\n",start)+2]
    start=source.index("static void store_sculpt_layer_disps(")
    start=source.index("\n{",start)+2
    capture=source[start:source.index("  const SculptSession &ss",start)]
    start=source.index("static SculptLayerPersistentRestoreResult sculpt_layer_undo_persistent_restore(")
    start=source.index("\n{",start)+2
    restore=source[start:source.index("  MultiresModifierData *mmd",start)]
    cpp=r'''
#include <cassert>
#include <cstdlib>
HELPER
bool sculpt_layer_undo_persistent_enabled() { return true; }
enum class Type { Position };
struct Frame { bool is_empty() const {return false;} int size() const {return 1;} };
struct Storage { bool sculpt_layer_undo=true; };
struct StepData { Type type=Type::Position; Storage *position_step_storage;
                  Frame layer_frame; int layer_frame_active=0; };
enum class SculptLayerPersistentRestoreResult { NotApplicable, PositionSwap };
void capture_probe(bool persistent_snapshot_valid, bool &copied) {
CAPTURE
  copied=true;
}
SculptLayerPersistentRestoreResult restore_probe(StepData &step_data) {
RESTORE
  return SculptLayerPersistentRestoreResult::PositionSwap;
}
int main() {
  Storage storage; StepData step{Type::Position,&storage,{},0};
  unsetenv("BLENDER_SCULPT_LAYER_UNDO_CCG");
  assert(sculpt_layer_undo_ccg_enabled());
  bool copied=false; capture_probe(true,copied); assert(!copied);
  assert(restore_probe(step)==SculptLayerPersistentRestoreResult::NotApplicable);
  setenv("BLENDER_SCULPT_LAYER_UNDO_CCG","0",1);
  assert(!sculpt_layer_undo_ccg_enabled());
  capture_probe(true,copied); assert(copied);
  assert(restore_probe(step)==SculptLayerPersistentRestoreResult::PositionSwap);
  copied=false; capture_probe(false,copied); assert(!copied);
  setenv("BLENDER_SCULPT_LAYER_UNDO_CCG","1",1);
  assert(sculpt_layer_undo_ccg_enabled());
}
'''.replace("HELPER",helper).replace("CAPTURE",capture).replace("RESTORE",restore)
    with tempfile.TemporaryDirectory() as temp:
        src=Path(temp)/"policy.cc"; exe=Path(temp)/"policy"
        src.write_text(cpp)
        subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


class CCGModel(Model):
    def stroke(self, delta):
        step=Undo(self.positions.copy(),[],self.weight,False)
        self.positions=[p+d for p,d in zip(self.positions,delta)]
        self.dirty=True
        return step


def simulation():
    cases=0
    for recorded in (F(1),F(1,2),F(-1),F(2)):
        for display in (recorded,F(1,4),F(-1,2),F(0)):
            model=CCGModel(True,recorded)
            deltas=([F(1),F(0),F(2)],[F(0),F(3),F(1)],[F(2),F(1),F(0)])
            steps=[model.stroke(delta) for delta in deltas]
            final=[p/recorded for p in model.positions]
            for _ in range(10):
                model.strength(display)
                for step in reversed(steps):
                    model.exchange(step)
                    assert model.coherent()
                model.commit(); assert model.payload==[F(0)]*3
                model.strength(recorded)
                for step in steps:
                    model.exchange(step)
                    assert model.coherent()
                model.commit(); assert model.payload==final
            cases+=1
    print(f"PASS: CCG policy/legacy guards; {cases} scenarios, 960 coordinate exchanges")


if __name__=="__main__":
    guards()
    simulation()
