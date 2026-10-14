#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
std::map<void*,size_t> allocations;
template<class T>T* MEM_new_array_zeroed(int n,const char*){T*p=new T[n]{};allocations[p]=n*sizeof(T);return p;}
template<class T>T* MEM_new_array_uninitialized(int n,const char*){T*p=new T[n];allocations[p]=n*sizeof(T);return p;}
template<class T>void MEM_delete(T*p){if(p){assert(allocations.erase(p));delete[] p;}}
#define MEM_SAFE_DELETE(p) do{MEM_delete(p);p=nullptr;}while(0)
struct MultiresColorGrid{float(*colors)[4]=nullptr;int totcolor=0,level=0;};
struct MultiresModifierData{MultiresColorGrid*sculpt_color_grids=nullptr;int sculpt_color_grids_num=0,sculpt_color_level=0,sculptlvl=3,totlvl=3;int shape_layers_sentinel=123,recording_sentinel=456;};
const char*BLI_getenv(const char*s){return std::getenv(s);}
namespace subdiv{int grid_size_from_level(int l){assert(l>=1&&l<=13);return(1<<(l-1))+1;}}
namespace bke::multires_layers {
void polypaint_clear(MultiresModifierData&);
bool polypaint_ensure(MultiresModifierData&,int,int);
bool polypaint_lazy_master_enabled();
bool polypaint_stage_master(const MultiresModifierData&,int,int,MultiresModifierData&);
void polypaint_swap_master(MultiresModifierData&,MultiresModifierData&);
}
struct SculptSession{void*cache=nullptr,*filter_cache=nullptr;};
struct Mesh{int corners_num=3;};
struct Runtime{SculptSession*sculpt_session=nullptr;};
struct Object{void*data=nullptr;Runtime*runtime=nullptr;MultiresModifierData*modifier=nullptr;int id=0;};
struct Scene{};
struct UserPrefs{int undosteps=32,uiflag=1;}U;
constexpr int USER_GLOBALUNDO=1;
struct UndoStep{const void*type=nullptr;};int memfile_type;
const void*BKE_UNDOSYS_TYPE_MEMFILE=&memfile_type;
struct UndoStack{UndoStep*step_init=nullptr,*step_active=nullptr;}undo_stack;
struct bContext{Object*object=nullptr;Scene*scene=nullptr;bool paint=true;};
struct wmOperator{void*reports=nullptr;};
std::array<UndoStep,32>undo_steps;
int pushes=0,fail_push=0,tags=0,notifiers=0;bool compatible=true;
std::vector<int>snapshots;
Object*CTX_data_active_object(bContext*C){return C->object;}
Scene*CTX_data_scene(bContext*C){return C->scene;}
const MultiresModifierData*BKE_sculpt_multires_active(Scene*,Object*o){return o->modifier;}
bool sculpt_polypaint_skips_geometry_rec(bContext*C){return C->paint;}
template<class T>T id_cast(void*p){return static_cast<T>(p);}
UndoStack*ED_undo_stack_get(){return &undo_stack;}
bool ED_undo_is_memfile_compatible(bContext*){return compatible;}
void ED_undo_push(bContext*C,const char*){++pushes;if(pushes==fail_push)return;assert(pushes<int(undo_steps.size()));undo_steps[pushes].type=BKE_UNDOSYS_TYPE_MEMFILE;undo_stack.step_active=&undo_steps[pushes];snapshots.push_back(C->object->modifier->sculpt_color_level);}
constexpr int RPT_WARNING=1,RPT_ERROR=2,ID_RECALC_SYNC_TO_EVAL=1,NC_OBJECT=1,ND_MODIFIER=2;
void BKE_report(void*,int,const char*){}
void DEG_id_tag_update(int*,int){++tags;}
void WM_event_add_notifier(bContext*,int,Object*){++notifiers;}
