/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "compact_native_brush_adapter.hh"
#include "physical_brush_intent.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace sculpt_physics;

int main()
{
  using Edge=std::pair<std::size_t,std::size_t>;
  const std::vector<Edge> edges{{0,1},{1,2},{2,3},{3,4},{4,5},{1,4},{0,5}};
  const std::vector<Vec3> initial{{0,0,0},{1,0,0},{2,0,0},{2,1,0},{1,1,0},{0,1,0}};
  auto current=initial;
  const auto edge_at=[&](std::size_t id) { return edges.at(id); };
  const auto mass=[](std::size_t id) { return id==0 ? 0.0 : 1.0; };
  const auto current_at=[&](std::size_t id) { return current.at(id); };
  World reference;
  for (std::size_t i=0;i<initial.size();++i) reference.add_particle(initial[i],mass(i));
  for (auto [a,b] : edges) reference.add_bond(a,b,1e-5);
  GpuTopologyBondIndex reference_index(reference,5);
  CompactBrushTopology compact(initial.size(),edges.size(),edge_at);
  std::unordered_map<std::size_t,Vec3> saved;
  auto rest_at=[&](std::size_t id) {
    const auto it=saved.find(id);
    return it==saved.end() ? current_at(id) : it->second;
  };
  for (int step=0;step<12;++step) {
    NativeBrushDab dab;
    dab.topology_generation=5;
    dab.assist=step%3==0 ? 0.0 : step%3==1 ? 0.5 : 1.0;
    dab.kind=step%2==0 ? NativeSculptBrushKind::Clay : NativeSculptBrushKind::Smooth;
    const auto profile=experimental_native_brush_profile(dab.kind);
    dab.compliance=profile.target_compliance;
    dab.material=profile.material;
    const std::size_t seed=1+std::size_t(step%5);
    std::vector<NativeBrushTranslation> samples{
      {seed,{0.02,0.03,-0.04}}, {0,{1,1,1}},
      {1+seed%5,{-0.01,0.02,0.01}}};
    auto full=prepare_cpu_native_brush_dab(reference,reference_index,samples,current_at,dab);
    auto small=prepare_compact_native_brush_dab(
        compact,samples,edge_at,rest_at,current_at,mass,dab);
    if(dab.assist==0.0){
      auto direct=prepare_compact_native_brush_dab(compact,samples,edge_at,rest_at,current_at,mass,dab,true);
      assert(!direct.prepared.has_solver());
      assert(direct.prepared.writes().size()==small.prepared.writes().size());
      for(std::size_t i=0;i<direct.prepared.writes().size();++i){
        const auto &x=direct.prepared.writes()[i],&y=small.prepared.writes()[i];
        assert(direct.local_to_global.at(x.global_index)==small.local_to_global.at(y.global_index));
        assert(x.position.x==y.position.x && x.position.y==y.position.y && x.position.z==y.position.z);
      }
      direct.prepared.finish_editor_success();
    }
    assert(full.writes().size()==small.prepared.writes().size());
    for (std::size_t i=0;i<full.writes().size();++i) {
      const auto &a=full.writes()[i];
      const auto &b=small.prepared.writes()[i];
      assert(a.global_index==small.local_to_global.at(b.global_index));
      assert(a.global_index!=0);
      assert(std::abs(a.position.x-b.position.x)<1e-12);
      assert(std::abs(a.position.y-b.position.y)<1e-12);
      assert(std::abs(a.position.z-b.position.z)<1e-12);
    }
    for (const auto &write : full.writes()) {
      saved.try_emplace(write.global_index,current.at(write.global_index));
      current.at(write.global_index)=write.position;
    }
    full.finish_editor_success();
    small.prepared.finish_editor_success();
  }
  // Compare cached and uncached real XPBD with current-rest Clay/Strips,
  // moving active sets, overlapping repeated dabs and a changing pin.
  CompactRegionCacheV27 cache;
  for(const auto kind:{NativeSculptBrushKind::Clay,NativeSculptBrushKind::ClayStrips}){
    current=initial;
    std::size_t pinned=0;
    const auto dynamic_mass=[&](std::size_t id){return id==pinned?0.0:1.0;};
    NativeBrushDab trial_dab;
    trial_dab.kind=kind;trial_dab.topology_generation=5;
    trial_dab.current_rest=true;trial_dab.assist=.75;
    const auto profile=experimental_native_brush_profile(kind);
    trial_dab.compliance=profile.target_compliance;trial_dab.material=profile.material;
    for(int step=0;step<16;++step){
      if(step==8)pinned=1;
      const auto seed=std::size_t(1+(step/4)%3);
      const std::vector<NativeBrushTranslation> targets{
          {seed,{.01,.02,.03}},{seed+1,{-.01,.01,.02}},{0,{.01,0,0}}};
      auto old=prepare_compact_native_brush_dab(compact,targets,edge_at,rest_at,current_at,dynamic_mass,trial_dab);
      auto fast=prepare_compact_native_brush_dab(compact,targets,edge_at,rest_at,current_at,dynamic_mass,trial_dab,false,&cache);
      assert(old.local_to_global==fast.local_to_global);
      assert(old.prepared.writes().size()==fast.prepared.writes().size());
      for(std::size_t i=0;i<old.prepared.writes().size();++i){
        const auto &x=old.prepared.writes()[i],&y=fast.prepared.writes()[i];
        assert(x.global_index==y.global_index);
        assert(fast.local_to_global.at(y.global_index)!=pinned);
        assert(x.position.x==y.position.x && x.position.y==y.position.y && x.position.z==y.position.z);
        current[fast.local_to_global.at(y.global_index)]=y.position;
      }
      old.prepared.finish_editor_success();fast.prepared.finish_editor_success();
    }
  }
  assert(cache.hits>0 && cache.misses>0);
  // Real assisted Clay and Strips: test checked dense lookup parity and
  // the post-blend relative edge safety with large opposing targets.
  for(const auto kind:{NativeSculptBrushKind::Clay,NativeSculptBrushKind::ClayStrips}){
    current=initial;
    NativeBrushDab trial;
    trial.kind=kind;trial.topology_generation=5;trial.current_rest=true;trial.assist=.65;
    trial.material=experimental_native_brush_profile(kind).material;
    const std::vector<NativeBrushTranslation> targets{{1,{0,0,2}},{2,{0,0,-2}},{0,{1,1,1}}};
    auto old=prepare_compact_native_brush_dab(compact,targets,edge_at,rest_at,current_at,mass,trial);
    trial.dense_ids_v28=true;
    auto fast=prepare_compact_native_brush_dab(compact,targets,edge_at,rest_at,current_at,mass,trial);
    assert(old.prepared.writes().size()==fast.prepared.writes().size());
    for(std::size_t i=0;i<old.prepared.writes().size();++i){
      const auto &a=old.prepared.writes()[i],&b=fast.prepared.writes()[i];
      assert(a.global_index==b.global_index);
      assert(a.position.x==b.position.x && a.position.y==b.position.y && a.position.z==b.position.z);
    }
    old.prepared.finish_editor_success();fast.prepared.finish_editor_success();
    trial.post_stability_v28=true;trial.stability.relative_limit=.35;
    auto guarded=prepare_compact_native_brush_dab(compact,targets,edge_at,rest_at,current_at,mass,trial);
    assert(!guarded.prepared.writes().empty());
    auto final=current;
    for(const auto &w:guarded.prepared.writes()){
      const auto id=guarded.local_to_global.at(w.global_index);
      assert(id!=0);final[id]=w.position;
    }
    for(const auto &[a,b]:edges){
      const auto dx=(final[b]-current[b])-(final[a]-current[a]);
      const auto e=current[b]-current[a];
      assert(std::hypot(dx.x,dx.y,dx.z)<=.35*std::hypot(e.x,e.y,e.z)+1e-10);
    }
    guarded.prepared.finish_editor_success();
  }
  current[0]=initial[0];
  assert(!saved.empty());
  assert(current[0].x==initial[0].x && current[0].y==initial[0].y);
  NativeBrushDab dab;
  dab.topology_generation=5;
  auto empty=prepare_compact_native_brush_dab(
      compact,{{0,{1,1,1}},{1,{0,0,0}}},edge_at,rest_at,current_at,mass,dab);
  assert(empty.prepared.writes().empty());
  bool rejected=false;
  try {
    (void)prepare_compact_native_brush_dab(
        compact,{{99,{0,0,0}}},edge_at,rest_at,current_at,mass,dab);
  }
  catch (const std::out_of_range &) { rejected=true; }
  assert(rejected);
  // Exercise the independent action through the ACTUAL compact XPBD path,
  // not just the pure target planner. Keep stroke-start coordinates stable.
  current=initial;
  saved.clear();
  PhysicalBrushIntentDab intent;
  intent.action=PhysicalBrushAction::Deposit;
  intent.center={1,0,0};
  intent.radius=3.0;
  intent.intensity=0.6;
  intent.dose=0.25;
  intent.spread=0.55;
  NativeBrushDab independent;
  independent.topology_generation=5;
  independent.compliance=1e-8;
  independent.material={100.0,0.95};
  for (int pass=0;pass<10;++pass) {
    std::vector<PhysicalBrushIntentSample> footprint;
    for (std::size_t i=0;i<current.size();++i) {
      footprint.push_back({i,current[i],i==5 ? 0.0 : 1.0,1.0,mass(i)>0.0});
    }
    const auto target=plan_physical_brush_intent(footprint,intent);
    const double prior=current[1].z;
    auto trial=prepare_compact_native_brush_dab(
        compact,target,edge_at,rest_at,current_at,mass,independent);
    assert(!trial.prepared.writes().empty());
    for (const auto &w : trial.prepared.writes()) {
      const auto id=trial.local_to_global.at(w.global_index);
      assert(id!=0 && id!=5);
      assert(std::isfinite(w.position.z));
      saved.try_emplace(id,current[id]);
      current[id]=w.position;
    }
    trial.prepared.finish_editor_success();
    assert(current[1].z>prior+0.005);
  }
  assert(current[1].z>0.1);
  assert(current[0].z==0.0 && current[5].z==0.0);
  intent.dose=0.0;
  assert(plan_physical_brush_intent({{1,current[1],1.0,1.0,true}},intent).empty());
  return 0;
}
