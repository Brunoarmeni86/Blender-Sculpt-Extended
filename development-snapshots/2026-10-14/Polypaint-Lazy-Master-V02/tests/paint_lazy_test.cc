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
namespace bke::multires_layers {
void polypaint_clear(MultiresModifierData &mmd)
{
  if (mmd.sculpt_color_grids) {
    for (int i = 0; i < mmd.sculpt_color_grids_num; i++) {
      MEM_SAFE_DELETE(mmd.sculpt_color_grids[i].colors);
    }
    MEM_delete(mmd.sculpt_color_grids);
  }
  mmd.sculpt_color_grids = nullptr;
  mmd.sculpt_color_grids_num = 0;
  mmd.sculpt_color_level = 0;
}

bool polypaint_ensure(MultiresModifierData &mmd, const int level, const int grids_num)
{
  if (level < 1 || level > 13 || grids_num < 0) {
    return false;
  }
  if (mmd.sculpt_color_grids) {
    /* A Sculpt Level change must NOT resample/destroy the high-res master.
     * Topology changes are handled by explicit remapping, never by clear(). */
    return mmd.sculpt_color_grids_num == grids_num &&
           mmd.sculpt_color_level >= 1 && mmd.sculpt_color_level <= 13;
  }
  if (grids_num > 0) {
    mmd.sculpt_color_grids = MEM_new_array_zeroed<MultiresColorGrid>(
        grids_num, "multires polypaint grids");
  }
  mmd.sculpt_color_grids_num = grids_num;
  mmd.sculpt_color_level = level;
  return true;
}

bool polypaint_lazy_master_enabled()
{
  const char *value = BLI_getenv("BLENDER_SCULPT_POLYPAINT_LAZY_MASTER");
  return value && value[0] == '1' && value[1] == '\0';
}

bool polypaint_stage_master(const MultiresModifierData &source,
                           const int level,
                           const int corners_num,
                           MultiresModifierData &destination)
{
  if (level < 1 || level > 13 || corners_num <= 0 ||
      destination.sculpt_color_grids || destination.sculpt_color_grids_num != 0 ||
      destination.sculpt_color_level != 0)
  {
    return false;
  }
  const int old_level = source.sculpt_color_level;
  if (source.sculpt_color_grids) {
    if (old_level < 1 || old_level > 13 || level <= old_level ||
        source.sculpt_color_grids_num != corners_num)
    {
      return false;
    }
    const int old_side = subdiv::grid_size_from_level(old_level);
    for (int corner = 0; corner < corners_num; corner++) {
      const MultiresColorGrid &grid = source.sculpt_color_grids[corner];
      if (!grid.colors) {
        continue;
      }
      if (grid.level != old_level || grid.totcolor != old_side * old_side) {
        return false;
      }
      for (int sample = 0; sample < grid.totcolor; sample++) {
        for (int channel = 0; channel < 4; channel++) {
          if (!std::isfinite(grid.colors[sample][channel])) {
            return false;
          }
        }
      }
    }
  }
  else if (old_level != 0 || source.sculpt_color_grids_num != 0) {
    return false;
  }

  /* Own all staged buffers until the editor's Undo boundary succeeds. Neither
   * the live master nor earlier Color Undo snapshots are resized in place. */
  if (!polypaint_ensure(destination, level, corners_num)) {
    return false;
  }
  const int new_side = subdiv::grid_size_from_level(level);
  const int old_side = source.sculpt_color_grids ? subdiv::grid_size_from_level(old_level) : 0;
  const int step = old_side ? (new_side - 1) / (old_side - 1) : 0;
  for (int corner = 0; corner < corners_num; corner++) {
    MultiresColorGrid &grid = destination.sculpt_color_grids[corner];
    grid.level = level;
    grid.totcolor = new_side * new_side;
    if (!source.sculpt_color_grids || !source.sculpt_color_grids[corner].colors) {
      continue;
    }
    const float (*old)[4] = source.sculpt_color_grids[corner].colors;
    grid.colors = MEM_new_array_uninitialized<float[4]>(grid.totcolor,
                                                       "staged polypaint master");
    for (int y = 0; y < new_side; y++) {
      const int y0 = y / step, y1 = std::min(y0 + 1, old_side - 1);
      const double fy = double(y % step) / step;
      for (int x = 0; x < new_side; x++) {
        const int x0 = x / step, x1 = std::min(x0 + 1, old_side - 1);
        if (x % step == 0 && y % step == 0) {
          memcpy(grid.colors[y * new_side + x], old[y0 * old_side + x0], sizeof(float[4]));
          continue;
        }
        const double fx = double(x % step) / step;
        for (int channel = 0; channel < 4; channel++) {
          const double a = double(old[y0 * old_side + x0][channel]) * (1.0 - fx) +
                           double(old[y0 * old_side + x1][channel]) * fx;
          const double b = double(old[y1 * old_side + x0][channel]) * (1.0 - fx) +
                           double(old[y1 * old_side + x1][channel]) * fx;
          grid.colors[y * new_side + x][channel] = float(a * (1.0 - fy) + b * fy);
        }
      }
    }
  }
  return true;
}

void polypaint_swap_master(MultiresModifierData &a, MultiresModifierData &b)
{
  std::swap(a.sculpt_color_grids, b.sculpt_color_grids);
  std::swap(a.sculpt_color_grids_num, b.sculpt_color_grids_num);
  std::swap(a.sculpt_color_level, b.sculpt_color_level);
}


}
using namespace bke::multires_layers;
static bool sculpt_polypaint_prepare_stroke_master(bContext *C, wmOperator *op)
{
  if (!sculpt_polypaint_skips_geometry_rec(C)) {
    return true;
  }
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);
  MultiresModifierData *mmd = const_cast<MultiresModifierData *>(
      BKE_sculpt_multires_active(scene, ob));
  if (!bke::multires_layers::polypaint_lazy_master_enabled()) {
    if (mmd && mmd->sculpt_color_grids && mmd->sculpt_color_level > 0 &&
        mmd->sculpt_color_level < mmd->totlvl)
    {
      BKE_report(op->reports, RPT_WARNING,
                 "Multires Polypaint: master promotion after Subdivide pending; original color preserved");
      return false;
    }
    return true;
  }
  const Mesh *mesh = id_cast<Mesh *>(ob->data);
  const SculptSession *ss = ob->runtime->sculpt_session;
  if (!mmd || mmd->sculptlvl < 1 || mmd->sculptlvl > mmd->totlvl ||
      mmd->sculptlvl > 13 || (ss && (ss->cache || ss->filter_cache)))
  {
    BKE_report(op->reports, RPT_ERROR, "Cannot prepare Paint resolution during another sculpt transaction");
    return false;
  }
  if (mmd->sculpt_color_grids && mmd->sculpt_color_level >= mmd->sculptlvl &&
      mmd->sculpt_color_level <= 13 && mmd->sculpt_color_grids_num == mesh->corners_num)
  {
    return true;
  }

  MultiresModifierData staged{};
  const int previous_level = mmd->sculpt_color_level;
  if (!bke::multires_layers::polypaint_stage_master(*mmd, mmd->sculptlvl,
                                                  mesh->corners_num, staged))
  {
    bke::multires_layers::polypaint_clear(staged);
    BKE_report(op->reports, RPT_ERROR, "Invalid Paint master; original colors preserved");
    return false;
  }
  const bool use_undo = U.undosteps > 0 && (U.uiflag & USER_GLOBALUNDO);
  UndoStack *stack = use_undo ? ED_undo_stack_get() : nullptr;
  if (use_undo && (!stack || stack->step_init || !ED_undo_is_memfile_compatible(C))) {
    bke::multires_layers::polypaint_clear(staged);
    BKE_report(op->reports, RPT_ERROR, "Paint promotion requires a completed Undo transaction");
    return false;
  }
  /* A full color-resolution change cannot cross older Color steps silently:
   * their saved grid strides differ. Preserve the old master in a Memfile
   * boundary before publishing. This happens once, outside stroke/parallel
   * node capture, and does not change Position Undo or first-stroke anchors. */
  if (use_undo) {
    UndoStep *before = stack->step_active;
    ED_undo_push(C, "Before Polypaint Resolution Change");
    if (!stack->step_active || stack->step_active == before ||
        stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE)
    {
      bke::multires_layers::polypaint_clear(staged);
      BKE_report(op->reports, RPT_ERROR, "Paint promotion checkpoint failed; original colors preserved");
      return false;
    }
  }
  bke::multires_layers::polypaint_swap_master(*mmd, staged);
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  if (use_undo) {
    UndoStep *before = stack->step_active;
    ED_undo_push(C, "Prepare Polypaint Resolution");
    if (!stack->step_active || stack->step_active == before ||
        stack->step_active->type != BKE_UNDOSYS_TYPE_MEMFILE)
    {
      bke::multires_layers::polypaint_swap_master(*mmd, staged);
      bke::multires_layers::polypaint_clear(staged);
      DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
      BKE_report(op->reports, RPT_ERROR, "Paint promotion Undo failed; original colors restored");
      return false;
    }
  }
  bke::multires_layers::polypaint_clear(staged);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);
  const char *trace = std::getenv("BLENDER_LAYER_RESP_TRACE");
  if (trace && trace[0] && trace[0] != '0') {
    std::printf("[POLYPAINT_LAZY_MASTER] from=%d to=%d geometry=%d undo_boundary=%d\n",
                previous_level, mmd->sculpt_color_level, mmd->totlvl, int(use_undo));
    std::fflush(stdout);
  }
  return true;
}

void populate(MultiresModifierData&m,int level){
 m.totlvl=7;m.sculptlvl=level;assert(polypaint_ensure(m,level,3));
 auto&g=m.sculpt_color_grids[0];int side=subdiv::grid_size_from_level(level);
 g.level=level;g.totcolor=side*side;g.colors=MEM_new_array_uninitialized<float[4]>(g.totcolor,"fixture");
 for(int i=0;i<g.totcolor;i++)for(int c=0;c<4;c++)g.colors[i][c]=float((i*71+c*23)%997)/996.f;
 g.colors[0][0]=-0.f;
}
void reset_undo(){undo_stack={};undo_steps={};undo_steps[0].type=&memfile_type;undo_stack.step_active=&undo_steps[0];pushes=fail_push=tags=notifiers=0;compatible=true;U={};snapshots.clear();}
int main(){
 setenv("BLENDER_SCULPT_POLYPAINT_LAZY_MASTER","1",1);int cases=0;
 for(int old=1;old<=5;old++)for(int target=old+1;target<=old+3;target++){
  MultiresModifierData m,staged;populate(m,old);auto*source=m.sculpt_color_grids[0].colors;
  int oldside=subdiv::grid_size_from_level(old),oldarea=oldside*oldside;
  std::vector<float>before(reinterpret_cast<float*>(source),reinterpret_cast<float*>(source)+oldarea*4);
  assert(polypaint_stage_master(m,target,3,staged));
  assert(m.sculpt_color_level==old && m.sculpt_color_grids[0].colors==source);
  assert(!std::memcmp(source,before.data(),before.size()*sizeof(float)));
  int newside=subdiv::grid_size_from_level(target),stride=(newside-1)/(oldside-1);
  for(int y=0;y<oldside;y++)for(int x=0;x<oldside;x++)assert(!std::memcmp(staged.sculpt_color_grids[0].colors[(y*stride)*newside+x*stride],source[y*oldside+x],sizeof(float[4])));
  for(int i=0;i<newside*newside;i++)for(int c=0;c<4;c++)assert(std::isfinite(staged.sculpt_color_grids[0].colors[i][c]));
  assert(!staged.sculpt_color_grids[1].colors&&!staged.sculpt_color_grids[2].colors);
  polypaint_swap_master(m,staged);assert(m.sculpt_color_level==target);assert(m.shape_layers_sentinel==123&&m.recording_sentinel==456&&m.totlvl==7);
  polypaint_swap_master(m,staged);assert(m.sculpt_color_grids[0].colors==source);assert(!std::memcmp(source,before.data(),before.size()*sizeof(float)));
  polypaint_clear(staged);polypaint_clear(m);assert(allocations.empty());cases++;
 }
 for(int bad=0;bad<6;bad++){
  MultiresModifierData m,staged;populate(m,2);
  if(bad==0)m.sculpt_color_grids[0].level=1;
  if(bad==1)m.sculpt_color_grids[0].totcolor--;
  if(bad==2)m.sculpt_color_grids_num=2;
  if(bad==3)m.sculpt_color_grids[0].colors[4][0]=std::numeric_limits<float>::quiet_NaN();
  int target=bad==4?1:bad==5?14:3;
  assert(!polypaint_stage_master(m,target,3,staged));assert(!staged.sculpt_color_grids);m.sculpt_color_grids_num=3;
  polypaint_clear(m);assert(allocations.empty());cases++;
 }
 for(int scenario=0;scenario<9;scenario++){
  reset_undo();MultiresModifierData m;populate(m,2);m.sculptlvl=3;m.totlvl=6;
  Mesh mesh;SculptSession ss;Runtime runtime{&ss};Object ob{&mesh,&runtime,&m};Scene scene;bContext context{&ob,&scene};wmOperator op;
  auto*original=m.sculpt_color_grids[0].colors;
  if(scenario==1)m.sculptlvl=1; // Lower-level Paint with finer geometry: no promotion.
  if(scenario==2)fail_push=1;
  if(scenario==3)fail_push=2;
  if(scenario==4)undo_stack.step_init=&undo_steps[0];
  if(scenario==5)compatible=false;
  if(scenario==6)ss.cache=&ss;
  if(scenario==7)U.uiflag=0;
  if(scenario==8)context.paint=false;
  bool success=sculpt_polypaint_prepare_stroke_master(&context,&op);
  bool expected=scenario==0||scenario==1||scenario==7||scenario==8;assert(success==expected);
  if(scenario==0){assert(m.sculpt_color_level==3);assert(snapshots==std::vector<int>({2,3}));}
  else if(scenario==7){assert(m.sculpt_color_level==3&&pushes==0);}
  else{assert(m.sculpt_color_level==2&&m.sculpt_color_grids[0].colors==original);}
  assert(m.totlvl==6&&m.shape_layers_sentinel==123&&m.recording_sentinel==456);
  polypaint_clear(m);assert(allocations.empty());cases++;
 }
 reset_undo();MultiresModifierData m;m.totlvl=7;m.sculptlvl=2;Mesh mesh;Runtime rt;Object ob{&mesh,&rt,&m};Scene scene;bContext ctx{&ob,&scene};wmOperator op;
 assert(sculpt_polypaint_prepare_stroke_master(&ctx,&op));assert(m.sculpt_color_level==2&&m.totlvl==7);assert(snapshots==std::vector<int>({0,2}));
 for(int i=0;i<3;i++)assert(!m.sculpt_color_grids[i].colors);polypaint_clear(m);assert(allocations.empty());cases++;
 std::cout<<"PASS "<<cases<<" actual staging/editor cases: retained bits, sparse/implicit colors, rollback, late promotion, checkpoint failures, geometry/layer isolation\n";
 std::cout<<"LIMIT: scene, ED_undo_push and allocator stubbed; native Undo/Redo must pass Windows tests\n";
}
