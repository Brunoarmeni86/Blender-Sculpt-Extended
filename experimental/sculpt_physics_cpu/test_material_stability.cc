/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_stability.hh"
#include "native_grab_executor_adapter.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>

using namespace sculpt_physics;

int main()
{
  const auto norm=[](Vec3 v) {return std::hypot(v.x,v.y,v.z);};
  GpuLocalIsland island;
  island.topology_generation=1;
  island.global_ids={0,1,2,3,4};
  island.positions={{0,0,0},{1,0,0},{2,0,0},{3,0,0},{4,0,0}};
  island.writable={false,true,true,true,false};
  island.inverse_masses={0,1,1,1,0};
  island.radii.assign(5,0.05);
  island.bonds={{0,1,1,1e-5},{1,2,1,1e-5},{2,3,1,1e-5},{3,4,1,1e-5}};
  MaterialStabilitySettings settings;
  settings.enabled=true;
  settings.relative_limit=0.2;
  std::vector<Vec3> violent{{4,4,4},{2,0,0},{-2,0,0},{0,1,0},{4,4,4}};
  auto stats=stabilize_surface_motion(island,violent,settings,true);
  assert(norm(violent.front())==0 && norm(violent.back())==0);
  assert(stats.final_scale>=0 && stats.final_scale<=1);
  assert(stats.peak_relative_motion<=0.2+1e-12);
  for (const auto &b : island.bonds) {
    assert(norm(violent[b.a]-violent[b.b])<=0.2+1e-12);
    for (int t=0;t<=10;++t) {
      const Vec3 edge=island.positions[b.a]-island.positions[b.b]+
          (violent[b.a]-violent[b.b])*(double(t)/10);
      assert(norm(edge)>=0.8-1e-12 && norm(edge)<=1.2+1e-12);
    }
  }
  // Uniform motion must not be attenuated on an unpinned component.
  auto mobile=island;
  mobile.writable.assign(5,true);
  mobile.inverse_masses.assign(5,1);
  std::vector<Vec3> rigid(5,{2,3,4});
  stabilize_surface_motion(mobile,rigid,settings,true);
  for (auto v : rigid) assert(norm(v-Vec3{2,3,4})<1e-12);
  // Test the final actual executor writeback over repeated excessive
  // increments. The limit is measured against the current dab geometry,
  // not the original stroke-start rest lengths.
  NativeBrushDab dab;
  dab.topology_generation=1;
  dab.stability=settings;
  dab.material={1.0,0.15};
  dab.compliance=1e-6;
  for (int pass=0;pass<20;++pass) {
    auto result=prepare_cpu_native_brush_island(island,
        {{1,{0.5,0,0}},{2,{-0.5,0,0}},{3,{0,0.5,0}}},dab);
    auto next=island.positions;
    assert(!result.writes().empty());
    for (const auto &w : result.writes()) {
      assert(w.global_index>0 && w.global_index<4);
      next.at(w.global_index)=w.position;
    }
    for (const auto &b : island.bonds) {
      const Vec3 old_edge=island.positions[b.a]-island.positions[b.b];
      const Vec3 new_edge=next[b.a]-next[b.b];
      assert(norm(new_edge-old_edge)<=0.2*norm(old_edge)+1e-10);
    }
    assert(norm(next.front()-island.positions.front())==0);
    assert(norm(next.back()-island.positions.back())==0);
    result.finish_editor_success();
    island.positions=next;
  }
  settings.relative_limit=std::numeric_limits<double>::quiet_NaN();
  bool rejected=false;
  try { stabilize_surface_motion(island,violent,settings,true); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
}
