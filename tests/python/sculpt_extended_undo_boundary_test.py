"""Compile actual boundary flush/trace fragments with minimal state types.

This checks ordering and failure retention, not Blender geometry or UndoStack.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]


def main():
    source=(ROOT/"source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc").read_text()
    start=source.index("  /* Native CCG undo can leave restored coordinates pending.")
    end=source.index("  mmd->sculpt_layer_active = step_data.layer_frame_active;",start)
    boundary=source[start:end+len("  mmd->sculpt_layer_active = step_data.layer_frame_active;")]
    start=source.index("static void sculpt_layer_undo_trace_state(")
    trace=source[start:source.index("\n}\n",start)+2]
    cpp=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
constexpr int OB_MESH=1, eModifierType_Multires=1, MULTIRES_SCULPT_LAYER_VISIBLE=1;
struct Mesh {int verts_num=8,faces_num=6,corners_num=24;};
struct SubdivCCG {int grids_num=24,grid_size=3;struct{bool coords=true,hidden=false,masks=false;}dirty;};
struct MDisps {float (*disps)[3]; int totdisp;};
struct MultiresSculptLayer {int grids_num=1; MDisps *disps; int flag=1;
                         float strength=0.5f; char name[16]="Layer";};
struct ModifierData {int type=1;char name[16]="Multires";};
struct MultiresModifierData:ModifierData {int sculptlvl=2,totlvl=4,sculpt_layer_active=0;
                                       std::vector<MultiresSculptLayer>sculpt_layers;};
struct SculptSession {SubdivCCG *subdiv_ccg;};
struct Runtime {SculptSession *sculpt_session;};
struct Object {int type=1,mode=2;void *data;Runtime *runtime;
               std::vector<MultiresModifierData>modifiers;
               int live_level=2;float strength=0.5f,surface=2,delta=0;
               bool pending=true,fail=false;};
struct bContext {Object *object;};
Object *CTX_data_active_object(bContext *C){return C->object;}
template<typename T>T id_cast(const void *value){return reinterpret_cast<T>(value);}
struct StepData {int type=1,layer_frame_active=1;struct{int grids_num=24,grid_size=3;}grids;};
struct SculptUndoStep {struct{char name[16]="Fill";}step;StepData data;};
bool multires_force_sculpt_rebuild(Object *object) {
  auto &mmd=object->modifiers[0];
  if(object->fail || mmd.sculptlvl!=object->live_level)return false;
  object->delta=object->surface/object->strength;
  object->pending=false;
  return true;
}
void prepare(Object *object,MultiresModifierData *mmd,int wanted_level,const StepData &step_data){
BOUNDARY
}
__TRACE_FUNCTION__
int main(){
  Mesh mesh; SubdivCCG ccg; SculptSession ss{&ccg};Runtime runtime{&ss};
  Object object;object.data=&mesh;object.runtime=&runtime;
  object.modifiers.emplace_back(); StepData step;
  prepare(&object,&object.modifiers[0],4,step);
  assert(!object.pending&&object.delta==4&&object.modifiers[0].sculptlvl==4);
  object.pending=true;object.delta=0;object.modifiers[0].sculptlvl=2;
  object.fail=true;
  prepare(&object,&object.modifiers[0],4,step);
  assert(object.pending&&object.delta==0&&object.modifiers[0].sculptlvl==2);
  object.fail=false;object.pending=true;object.modifiers[0].sculptlvl=2;
  prepare(&object,&object.modifiers[0],2,step);
  assert(!object.pending&&object.delta==4);
  float values[3][3]={{1,0,0},{1,0,0},{1,0,0}};MDisps grid{values,3};
  MultiresSculptLayer layer;layer.disps=&grid;
  object.modifiers[0].sculpt_layers.push_back(layer);
  bContext context{&object};SculptUndoStep undo;
  unsetenv("BLENDER_SCULPT_LAYER_UNDO_TRACE");
  sculpt_layer_undo_trace_state(&context,undo,"DISABLED");
  setenv("BLENDER_SCULPT_LAYER_UNDO_TRACE","1",1);
  sculpt_layer_undo_trace_state(&context,undo,"PROBE");
  assert(object.delta==4&&object.surface==2&&layer.strength==0.5f&&values[0][0]==1);
}
'''.replace("BOUNDARY",boundary).replace("__TRACE_FUNCTION__",trace)
    with tempfile.TemporaryDirectory() as temp:
        src=Path(temp)/"boundary.cc"; exe=Path(temp)/"boundary"
        src.write_text(cpp)
        subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
        output=subprocess.check_output([str(exe)],text=True)
        assert "OLD_FRAME_FLUSH_FAILED" in output
        assert "[UNDO_STATE] event=PROBE" in output
        assert "energy=3" in output
        assert "DISABLED" not in output
    print("PASS: old-level flush, old-weight solve, failed flush retention, bounded read-only trace")


if __name__=="__main__":main()
