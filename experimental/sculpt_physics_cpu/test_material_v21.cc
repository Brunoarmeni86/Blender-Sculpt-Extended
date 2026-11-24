/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#include "adaptive_material_topology.hh"
#include "material_history_v21.hh"
#include "material_point_reference_v21.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>

using namespace sculpt_physics;

int main()
{
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  constexpr int n=9;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
    const double px=.1*(x-4),py=.1*(y-4);
    const double w=x==0||y==0||x==n-1||y==n-1?0:std::exp(-5*(px*px+py*py));
    v.push_back({{px,py,.06*std::exp(-20*(px*px+py*py))},w});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x) {
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  ClaySurfaceDab dab;
  dab.radius=.7;dab.dose=.15;dab.intensity=.9;dab.mobility=.8;
  dab.action=ClaySurfaceAction::FusionDeposit;
  const auto dep=solve_material_transport_v21(v,t,dab,4);
  assert(dep.substeps==4 && dep.surface.signed_swept_volume>0);
  for(std::size_t i=0;i<v.size();++i) v[i].position+=dep.surface.displacement[i];
  dab.direction={1,0,0};dab.action=ClaySurfaceAction::Push;
  dab.friction=0;dab.flow_fusion=false;
  assert(solve_material_transport_v21(v,t,dab).surface.transported_volume==0);
  dab.friction=1;
  assert(solve_material_transport_v21(v,t,dab).surface.transported_volume>0);
  for(auto action:{ClaySurfaceAction::Push,ClaySurfaceAction::Compress,
                   ClaySurfaceAction::Press,ClaySurfaceAction::PressureFlow,
                   ClaySurfaceAction::HybridFlow,ClaySurfaceAction::Compact}) {
    dab.action=action;dab.volume_preservation=1;
    for(int k=0;k<4;++k) {
      const auto result=solve_material_transport_v21(v,t,dab);
      assert(std::abs(result.surface.signed_swept_volume)<1e-10);
      for(std::size_t i=0;i<v.size();++i) {
        const auto d=result.surface.displacement[i];
        assert(d.x==0 && d.y==0 && std::isfinite(d.z));
        if(v[i].influence==0) assert(d.z==0);
        v[i].position+=d;
      }
    }
  }

  MaterialPatch patch;
  patch.vertices={{{0,0,0},0},{{1,0,0},0},{{0,1,0},0},{{1,1,0},0}};
  MaterialPatchFace f0,f1;
  f0.vertex={0,1,2};f0.uv={UV2{0,0},UV2{1,0},UV2{0,1}};f0.face_set=7;
  f1.vertex={2,1,3};f1.uv={UV2{2,2},UV2{3,2},UV2{3,3}};f1.face_set=8;
  patch.faces={f0,f1};
  const auto split=split_material_edge(patch,1,2);
  assert(patch.vertices.size()==4 && split.vertices.size()==5 && split.faces.size()==4);
  assert(split.vertices.back().source[0]==1 && split.vertices.back().source[1]==2);
  assert(split.faces[0].uv[1].u==.5 && split.faces[2].uv[1].u==2.5);
  assert(split.faces[0].face_set==7 && split.faces[2].face_set==8);
  const auto cut=separate_material_region(patch,{true,false});
  assert(cut.vertices.size()==6 && cut.faces[0].vertex[1]!=cut.faces[1].vertex[1]);
  assert(cut.faces[0].uv[1].u==f0.uv[1].u);

  MaterialHistory history{7,{{2,1,0,0},{1,1,0,0}}};
  auto after=stage_material_history(history,7,{{0,-.3,.9,.1},{1,.3,1,0}});
  assert(history.cells[0].mass==2 && after.cells[0].compaction>0);
  assert(std::abs(after.cells[0].mass+after.cells[1].mass-3)<1e-12);
  bool rejected=false;
  try {stage_material_history(history,8,{});} catch(const std::invalid_argument &) {rejected=true;}
  assert(rejected);

  MaterialPointV21 point;
  point.position={.25,.25,.25};point.velocity={.1,.2,.3};
  MaterialPointSettingsV21 settings;
  auto step=step_material_points_v21({point},settings);
  assert(step.grid_nodes==8 && std::abs(step.grid_mass-1)<1e-12);
  assert(std::abs(step.particles[0].position.x-(.25+.1*settings.dt))<1e-12);
  assert(std::abs(step.particles[0].volume-1)<1e-12);
  MaterialPointToolV21 tool;tool.enabled=true;tool.center={-.5,0,0};
  tool.radius=.75;tool.velocity={1,0,0};
  step=step_material_points_v21({point},settings,tool);
  assert(step.contact_nodes>0 && std::abs(step.grid_mass-1)<1e-12);
  settings.dt=100;
  rejected=false;
  try {step_material_points_v21({point},settings);} catch(const std::invalid_argument &) {rejected=true;}
  assert(rejected);
}
