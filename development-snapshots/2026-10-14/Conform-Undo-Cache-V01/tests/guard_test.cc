// SPDX-License-Identifier: GPL-2.0-or-later
// Actual object writeback body + actual inserted decode guard. Geometry/ID APIs
// are modeled; this is not a complete Blender/Windows integration test.
#include "../BKE_multires_undo_cache_guard.hh"
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>
#include <thread>
#define BLI_assert(x) assert(x)
enum {OB_MESH=1,eModifierType_Multires=1,CD_MDISPS=1,CD_GRID_PAINT_MASK=2,STEP_INVALID=0};
struct Dirty{bool coords=true,hidden=false,masks=false;};
struct SubdivCCG{Dirty dirty;int level=2;float surface=7;float mask=.8f;};
struct Layers{bool empty=false;bool is_empty()const{return empty;}};
struct ModifierData{int type=eModifierType_Multires;};
struct MultiresModifierData:ModifierData{Layers sculpt_layers;};
struct Mods{std::vector<ModifierData*> items;struct Iterator{std::vector<ModifierData*>::iterator i;ModifierData&operator*(){return **i;}Iterator&operator++(){++i;return*this;}bool operator!=(const Iterator&o){return i!=o.i;}};Iterator begin(){return{items.begin()};}Iterator end(){return{items.end()};}};
struct MeshRuntime{std::unique_ptr<SubdivCCG> subdiv_ccg;int subdiv_ccg_tot_level=5;};
struct Mesh{MeshRuntime*runtime;float base=5,mask=.2f;};
struct SculptSession{SubdivCCG*subdiv_ccg;};
struct Runtime{bool is_data_eval_owned=true;Mesh*data_orig;SculptSession*sculpt_session;};
struct Object{int type=OB_MESH;Runtime*runtime;Mods modifiers;Mesh*data;Object*original;Mesh*evaluated;};
struct Objects{std::vector<Object*>items;struct Iterator{std::vector<Object*>::iterator i;Object&operator*(){return **i;}Iterator&operator++(){++i;return*this;}bool operator!=(const Iterator&o){return i!=o.i;}};Iterator begin(){return{items.begin()};}Iterator end(){return{items.end()};}};
struct Main{Objects objects;};
namespace blender {
template<class T>T id_cast(Mesh*p){return static_cast<T>(p);}
static int reshapes=0,masks=0,copies=0;
Mesh*BKE_object_get_evaluated_mesh_no_subsurf_unchecked(Object*o){return o->evaluated;}
Object*DEG_get_original(Object*o){return o->original;}
bool multires_flush_mask_updates(SubdivCCG&c,Mesh&m,int){m.mask=c.mask;++masks;return true;}
bool multiresModifier_reshapeFromCCG(int,Mesh*m,SubdivCCG*c){m->base=c->surface;++reshapes;return true;}
void copy_ccg_data(Mesh*d,Mesh*s,int type){++copies;if(type==CD_MDISPS)d->base=s->base;else d->mask=s->mask;}
#include "patched_object_update.inc"
void simulated_memfile_decode(Main*bmain,int undo_direction, bool call_cleanup)
{
#include "decode_guard.inc"
  if(call_cleanup)for(Object&o:bmain->objects)object_update_from_subsurf_ccg(o.evaluated?&o:o.original);
}
}
using namespace blender;
int main(){
 int cases=0;MeshRuntime er,orr,cr;er.subdiv_ccg=std::make_unique<SubdivCCG>();
 Mesh original{&orr},cow{&cr},evaluated{&er};SculptSession ss{er.subdiv_ccg.get()};
 Runtime rt{true,&cow,&ss};MultiresModifierData md;
 Object orig{OB_MESH,&rt,{{&md}},&original,nullptr,&evaluated};orig.original=&orig;
 Object eval=orig;eval.original=&orig;
 Main main{{{&orig}}};
 auto reset=[&](){original.base=5;original.mask=.2f;cow.base=5;cow.mask=.2f;ss.subdiv_ccg=er.subdiv_ccg.get();
 er.subdiv_ccg->dirty={true,false,false};er.subdiv_ccg->surface=7;md.sculpt_layers.empty=false;
 rt.sculpt_session=&ss;rt.is_data_eval_owned=true;reshapes=masks=copies=0;unsetenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD");
 assert(!bke::multires_layers::undo_cache::discard_active());};
 // Reproduce native double contribution: composed 7 becomes base, layer 2 remains.
 reset();object_update_from_subsurf_ccg(&eval);assert(original.base+2==9&&reshapes==1);++cases;
 for(int direction:{-1,1}){
  reset();setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
  simulated_memfile_decode(&main,direction,false);
  // Deferred depsgraph cleanup occurs after the RAII scope has ended.
  assert(!bke::multires_layers::undo_cache::discard_active());
  object_update_from_subsurf_ccg(&eval);assert(original.base+2==7&&reshapes==0);++cases;
 }
 for(int channel=0;channel<3;channel++){
  reset();er.subdiv_ccg->dirty={channel==0,channel==1,channel==2};
  setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
  simulated_memfile_decode(&main,-1,true);
  assert(original.base==5&&original.mask==.2f&&reshapes==0&&masks==0);++cases;
 }
 // Caches not linked to the original SculptSession are covered during decode.
 reset();rt.sculpt_session=nullptr;setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
 {bke::multires_layers::undo_cache::ScopedDiscardCacheWrites guard;object_update_from_subsurf_ccg(&eval);}
 assert(original.base==5&&reshapes==0&&er.subdiv_ccg->dirty.coords);++cases;
 // Native no-layer behavior remains available, even inside the scope.
 reset();md.sculpt_layers.empty=true;setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
 simulated_memfile_decode(&main,-1,false);object_update_from_subsurf_ccg(&eval);
 assert(original.base==7&&reshapes==1);++cases;
 for(const char*flag:{"0","10",""}){
  reset();setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD",flag,1);
  simulated_memfile_decode(&main,-1,false);object_update_from_subsurf_ccg(&eval);
  assert(original.base==7&&reshapes==1);++cases;
 }
 reset();setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
 {bke::multires_layers::undo_cache::ScopedDiscardCacheWrites outer;
  {bke::multires_layers::undo_cache::ScopedDiscardCacheWrites inner;
   assert(bke::multires_layers::undo_cache::discard_active());}
  assert(bke::multires_layers::undo_cache::discard_active());
  std::thread worker([&](){assert(bke::multires_layers::undo_cache::discard_active());object_update_from_subsurf_ccg(&eval);});worker.join();
  assert(original.base==5&&reshapes==0);++cases;}
 assert(!bke::multires_layers::undo_cache::discard_active());++cases;
 reset();setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
 try{bke::multires_layers::undo_cache::ScopedDiscardCacheWrites guard;throw 1;}catch(int){}
 assert(!bke::multires_layers::undo_cache::discard_active());++cases;
 // Outside global decode, an ordinary dirty Sculpt surface remains pending.
 reset();setenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD","1",1);
 object_update_from_subsurf_ccg(&eval);assert(reshapes==1);++cases;
 std::printf("PASS %d actual-writeback/decode-guard cases; Blender geometry and ID APIs modeled\n",cases);
}
