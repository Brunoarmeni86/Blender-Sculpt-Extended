/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "compact_native_brush_adapter.hh"

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
  return 0;
}
