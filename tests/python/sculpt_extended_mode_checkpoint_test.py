#!/usr/bin/env python3
"""Compile the actual boundary helpers against mocked Blender services.

This checks routing, commit/snapshot order and redo-tail preservation. It does
not exercise Blender's memfile serialization or native undo decode.
Run with a C++17 compiler available: python this_file.py [--sanitize].
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MOCKS = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <string>
constexpr int OB_MESH=1, OB_MODE_SCULPT=2, USER_GLOBALUNDO=1;
constexpr int eModifierType_Multires=3, OPTYPE_UNDO=1, OPTYPE_UNDO_GROUPED=2;
struct UndoType {};
UndoType sculpt_type, memfile_type, other_type;
const UndoType *BKE_UNDOSYS_TYPE_SCULPT=&sculpt_type;
const UndoType *BKE_UNDOSYS_TYPE_MEMFILE=&memfile_type;
struct UndoStep {
  UndoStep *next=nullptr, *prev=nullptr;
  char name[64]="Stroke";
  const UndoType *type=&sculpt_type;
  bool skip=false, use_memfile_step=false, use_old_bmain_data=true;
  int snapshot=0;
};
template<class T> struct ListBaseT { T *first=nullptr, *last=nullptr; };
template<class T> void BLI_remlink(ListBaseT<T> *list,T *item) {
  if(item->prev) item->prev->next=item->next; else list->first=item->next;
  if(item->next) item->next->prev=item->prev; else list->last=item->prev;
  item->prev=item->next=nullptr;
}
template<class T> void BLI_addtail(ListBaseT<T> *list,T *item) {
  item->prev=list->last; item->next=nullptr;
  if(list->last) list->last->next=item; else list->first=item;
  list->last=item;
}
struct UndoStack {
  ListBaseT<UndoStep> steps;
  UndoStep *step_active=nullptr, *step_active_memfile=nullptr, *step_init=nullptr;
  int group_level=0;
};
struct Layers : std::vector<int> { Layers():std::vector<int>{1}{} bool is_empty() const {return empty();} };
struct ModifierData { int type=eModifierType_Multires; Layers sculpt_layers; };
using MultiresModifierData=ModifierData;
struct Object { int type=OB_MESH, mode=OB_MODE_SCULPT; struct {void *lib=nullptr;} id;
  std::vector<ModifierData> modifiers{ModifierData{}}; };
struct Runtime { UndoStack *undo_stack=nullptr; };
struct wmWindowManager { int op_undo_depth=0; Runtime *runtime=nullptr; };
struct bContext {wmWindowManager *wm=nullptr; Object *ob=nullptr;};
struct wmOperatorType {const char *idname; int flag;};
struct UserDef {int undosteps=32,uiflag=USER_GLOBALUNDO;} U;
wmWindowManager *CTX_wm_manager(bContext *c){return c->wm;}
Object *CTX_data_active_object(bContext *c){return c->ob;}
int LOG=0;
#define CLOG_ERROR(log,...) ((void)(log))
#define UNDO_NESTED_ASSERT(value) ((void)0)
#define STRNCPY(dst,src) std::snprintf(dst,sizeof(dst),"%s",src)
enum eUndoPushReturn {UNDO_PUSH_RET_FAILURE=0,UNDO_PUSH_RET_SUCCESS=1};
int commits=0,encodes=0,persistent=0,live=17;
bool commit_ok=true,push_ok=true;
std::vector<std::string> events;
std::vector<std::unique_ptr<UndoStep>> allocated;
namespace bke::multires_layers {
bool commit(Object &) {
  ++commits;events.push_back("commit");
  if(!commit_ok)return false;
  persistent=live;return true;
}}
eUndoPushReturn BKE_undosys_step_push_with_type(UndoStack *s,bContext *,const char *name,const UndoType *type) {
  ++encodes;events.push_back("encode");
  if(!push_ok)return UNDO_PUSH_RET_FAILURE;
  allocated.push_back(std::make_unique<UndoStep>());
  auto *us=allocated.back().get();us->type=type;us->snapshot=persistent;
  std::snprintf(us->name,sizeof(us->name),"%s",name);
  BLI_addtail(&s->steps,us);s->step_active=s->step_active_memfile=us;
  return UNDO_PUSH_RET_SUCCESS;
}
void undosys_stack_validate(UndoStack *s,bool) {
  assert(s->step_active && !s->step_active->skip);
  UndoStep *prev=nullptr;
  for(auto *p=s->steps.first;p;p=p->next){assert(p->prev==prev);prev=p;}
  assert(prev==s->steps.last);
}
bool ED_undo_sculpt_controls_begin(bContext *,bool=false);
'''

CASES = r'''
struct Fixture {
  UndoStep previous,stroke,redo1,redo2;
  UndoStack stack;
  Runtime runtime;
  wmWindowManager wm;
  Object ob;
  bContext ctx;
  Fixture(bool redo=false) {
    commits=encodes=0;persistent=0;live=17;commit_ok=push_ok=true;events.clear();
    U={32,USER_GLOBALUNDO};unsetenv("BLENDER_SCULPT_LAYER_UNDO_CONTROLS");
    unsetenv("BLENDER_LAYER_RESP_TRACE");
    previous.type=&memfile_type;
    BLI_addtail(&stack.steps,&previous);BLI_addtail(&stack.steps,&stroke);
    stack.step_active=&stroke;stack.step_active_memfile=&previous;
    if(redo){redo2.type=&memfile_type;BLI_addtail(&stack.steps,&redo1);BLI_addtail(&stack.steps,&redo2);}
    runtime.undo_stack=&stack;wm.runtime=&runtime;ctx={&wm,&ob};
  }
  void run(const char *id="OBJECT_OT_mode_set",int flag=0) {
    wmOperatorType ot{id,flag};wm_sculpt_controls_begin(&ctx,&ot);
  }
};
int main() {
  // Reproduce the old routing: mode_set(flag=0) and sculpt toggle are excluded.
  auto old_admits=[](const char *id,int flag){
    return (flag&(OPTYPE_UNDO|OPTYPE_UNDO_GROUPED)) &&
      !(std::strncmp(id,"SCULPT_OT_",10)==0 && std::strncmp(id,"SCULPT_OT_multires_layer_",25)!=0);
  };
  assert(!old_admits("OBJECT_OT_mode_set",0));
  assert(!old_admits("SCULPT_OT_sculptmode_toggle",OPTYPE_UNDO));
  const char *modes[]={"OBJECT_OT_mode_set","OBJECT_OT_mode_set_with_submode",
                       "OBJECT_OT_editmode_toggle","SCULPT_OT_sculptmode_toggle"};
  for(auto *id:modes){
    Fixture f(true);f.run(id);assert(commits==1&&encodes==1);
    assert((events==std::vector<std::string>{"commit","encode"}));
    auto *saved=f.stack.step_active;
    assert(saved->snapshot==17&&saved->type==&memfile_type);
    assert(f.stroke.skip&&f.stroke.use_memfile_step);
    assert(saved->next==&f.redo1&&f.redo1.next==&f.redo2);
    assert(!saved->use_old_bmain_data&&!f.redo1.use_old_bmain_data&&!f.redo2.use_old_bmain_data);
    // A nested mode operator must not create a second checkpoint.
    f.run("SCULPT_OT_sculptmode_toggle",OPTYPE_UNDO);assert(commits==1&&encodes==1);
    // Destructive edit may clear current grids, but the paired snapshot retains detail.
    persistent=live=0;assert(saved->snapshot==17);
  }
  {Fixture f;commit_ok=false;f.run();assert(commits==1&&encodes==0);assert(!f.stroke.skip&&f.stack.step_active==&f.stroke);}
  {Fixture f(true);push_ok=false;f.run();assert(commits==1&&encodes==1);
   assert(f.stack.step_active==&f.stroke&&f.stack.step_active_memfile==&f.previous);
   assert(f.stroke.next==&f.redo1&&f.redo1.next==&f.redo2&&!f.stroke.skip);}
  // Exclusions must leave pending CCG detail and the undo stack untouched.
  for(int i=0;i<13;++i){Fixture f;
    switch(i){
      case 0:f.ob.mode=0;break;case 1:f.ob.modifiers.clear();break;
      case 2:f.ob.modifiers[0].sculpt_layers.clear();break;
      case 3:f.wm.op_undo_depth=1;break;case 4:f.stack.step_init=&f.redo1;break;
      case 5:f.stack.group_level=1;break;case 6:f.stack.step_active=nullptr;break;
      case 7:f.stroke.type=&other_type;break;case 8:f.stroke.skip=true;break;
      case 9:f.stroke.use_memfile_step=true;break;case 10:U.undosteps=0;break;
      case 11:U.uiflag=0;break;case 12:f.runtime.undo_stack=nullptr;break;
    }
    f.run();assert(commits==0&&encodes==0&&persistent==0&&live==17);
  }
  {Fixture f;f.run("SCULPT_OT_brush_stroke",OPTYPE_UNDO);assert(commits==0&&encodes==0);}
  {Fixture f;f.run("OBJECT_OT_modifier_apply",OPTYPE_UNDO);assert(commits==0&&encodes==0);}
  {Fixture f;setenv("BLENDER_SCULPT_LAYER_UNDO_CONTROLS","1",1);
   f.run("SCULPT_OT_multires_layer_commit",OPTYPE_UNDO);assert(commits==0&&encodes==1);}
  std::puts("Mode checkpoint harness passed: actual routing/editor/pairing helpers; mocked commit and serialization.");
}
'''

source = MOCKS + function("source/blender/blenkernel/intern/undo_system.cc", "bool BKE_undosys_sculpt_step_pair_memfile(")
source += function("source/blender/editors/undo/ed_undo.cc", "bool ED_undo_sculpt_controls_begin(")
source += function("source/blender/windowmanager/intern/wm_event_system.cc", "static void wm_sculpt_controls_begin(")
source += CASES
with tempfile.TemporaryDirectory() as temp:
    cpp = pathlib.Path(temp) / "checkpoint.cc"
    binary = pathlib.Path(temp) / "checkpoint"
    cpp.write_text(source)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
    if "--sanitize" in sys.argv:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(["g++", *flags, str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
