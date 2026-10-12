"""Extract C++ routing guards and simulate persistent/CCG undo pairing.

The scalar model isolates synchronization defects; it does not simulate Blender
reshape geometry, PBVH, BMesh, allocation, or native UndoStack traversal.
"""
from dataclasses import dataclass
from fractions import Fraction as F
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def compile_guards():
    source = (ROOT / "source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc").read_text()
    helper = ""
    helper_start = source.find("static bool sculpt_layer_undo_ccg_enabled()")
    if helper_start >= 0:
        helper = source[helper_start:source.index("\n}\n", helper_start)+2]
    start = source.index("      us->data.layer_persistent_snapshot_valid =")
    expression = source[start:source.index(";", start)+1]
    start = source.index("    if (mmd->sculptlvl < mmd->totlvl || !step_data.layer_position_buffers_match_payload)")
    refresh = source[start:source.index(" {", start)]
    cpp = r'''
#include <cassert>
#include <cstdlib>
HELPER
struct Data { bool native_multires_base_undo=false; bool layer_persistent_snapshot_valid=false;
              bool layer_position_buffers_match_payload=true; };
struct Step { Data data; };
struct Modifier { int sculptlvl=3; int totlvl=3; };
struct CCG { int level=3; struct { bool coords=false, hidden=false, masks=false; } dirty; };
bool eligible(Step *us, const Modifier &mmd, const CCG &ccg) {
EXPRESSION
  return us->data.layer_persistent_snapshot_valid;
}
bool requires_refresh(const Modifier *mmd, const Data &step_data) {
REFRESH return true;
  return false;
}
int main() {
  setenv("BLENDER_SCULPT_LAYER_UNDO_CCG", "0", 1);
  for (int bits=0; bits<32; ++bits) {
    Step step; Modifier modifier; CCG ccg;
    step.data.native_multires_base_undo = bits & 1;
    ccg.dirty.coords = bits & 2; ccg.dirty.hidden = bits & 4; ccg.dirty.masks = bits & 8;
    if (bits & 16) modifier.sculptlvl=ccg.level=2;
    assert(eligible(&step,modifier,ccg) == (bits==0));
  }
  Step step; Modifier modifier; CCG ccg;
  ccg.level=2;
  assert(!eligible(&step,modifier,ccg));
  Data data;
  assert(!requires_refresh(&modifier,data));
  data.layer_position_buffers_match_payload=false;
  assert(requires_refresh(&modifier,data));
  data.layer_position_buffers_match_payload=true; modifier.sculptlvl=2;
  assert(requires_refresh(&modifier,data));
}
'''.replace("EXPRESSION",expression).replace("REFRESH",refresh).replace("HELPER",helper)
    with tempfile.TemporaryDirectory() as temp:
        src=Path(temp)/"guards.cc"; exe=Path(temp)/"guards"
        src.write_text(cpp)
        subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


@dataclass
class Undo:
    positions: list
    payload: list
    weight: F
    persistent: bool
    paired: bool = True


class Model:
    def __init__(self, fixed, weight=F(1)):
        self.fixed=fixed; self.weight=weight
        self.payload=[F(0)]*3; self.positions=[F(0)]*3; self.dirty=False

    def commit(self):
        if self.dirty:
            assert self.weight != 0
            self.payload=[p/self.weight for p in self.positions]
            self.dirty=False

    def recompose(self):
        assert not self.dirty
        self.positions=[p*self.weight for p in self.payload]

    def strength(self, weight):
        self.commit(); self.weight=weight; self.recompose()

    def stroke(self, delta):
        step=Undo(self.positions.copy(),self.payload.copy(),self.weight,
                  not self.dirty if self.fixed else True)
        self.positions=[p+d for p,d in zip(self.positions,delta)]
        self.dirty=True
        return step

    def exchange(self, step):
        same=self.weight==step.weight
        if step.persistent:
            self.commit()
            self.payload,step.payload=step.payload,self.payload
            if same and (step.paired or not self.fixed):
                self.positions,step.positions=step.positions,self.positions
                self.dirty=False
            else:
                self.recompose()
                if self.fixed: step.paired=False
        elif same:
            self.positions,step.positions=step.positions,self.positions
            self.dirty=True
        else:
            self.commit(); current=self.weight; self.weight=step.weight; self.recompose()
            self.positions,step.positions=step.positions,self.positions
            self.dirty=True; self.commit()
            self.weight=current; self.recompose()

    def coherent(self):
        return self.dirty or self.positions==[p*self.weight for p in self.payload]


def simulate():
    for fixed in (False,True):
        model=Model(fixed)
        first=model.stroke([F(1),F(0),F(2)])
        second=model.stroke([F(3),F(4),F(0)])
        model.exchange(second)
        assert model.coherent()==fixed
        if fixed:
            assert model.positions==[F(1),F(0),F(2)]
            model.exchange(first); assert model.positions==[F(0)]*3
            model.exchange(first); model.exchange(second)
            model.commit(); assert model.payload==[F(4),F(4),F(2)]
        print("pending two-stroke simulation:", "coherent" if fixed else "pre-fix mismatch reproduced")

    for fixed in (False,True):
        model=Model(fixed); step=model.stroke([F(1),F(2),F(3)])
        model.strength(F(1,2)); model.exchange(step)
        model.strength(F(1)); model.exchange(step)
        assert model.coherent()==fixed
        if fixed: assert model.positions==[F(1),F(2),F(3)]
        print("weight Undo -> original-weight Redo:", "coherent" if fixed else "pre-fix mismatch reproduced")

    cases=0
    for recorded in (F(1),F(1,2),F(2),F(-1,2)):
        for changed in (F(1,4),F(3,2),F(-1),F(0)):
            for pending in (False,True):
                model=Model(True,recorded)
                if pending: model.stroke([F(2),F(0),F(1)])
                before=[p/recorded for p in model.positions]
                step=model.stroke([F(1),F(2),F(3)])
                after=[p/recorded for p in model.positions]
                for _ in range(5):
                    model.strength(changed); model.exchange(step)
                    assert model.positions==[p*changed for p in before]
                    assert model.coherent()
                    model.strength(recorded); model.exchange(step)
                    assert model.positions==[p*recorded for p in after]
                    assert model.coherent()
                cases+=1
    print(f"PASS: {cases} scenarios, 160 Undo/Redo round trips (320 exchanges); exact rational arithmetic")


if __name__=="__main__":
    compile_guards()
    simulate()
