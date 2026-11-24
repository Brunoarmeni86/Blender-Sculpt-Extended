/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_grab_executor_adapter.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;

int main()
{
  World rest;
  std::vector<Vec3> editor;
  for (int i=0;i<21;++i) {
    // One endpoint is a fixed mask/hidden vertex.
    const Vec3 p{double(i),i==10 ? -0.2 : 0.0,0.0};
    rest.add_particle(p,i==10 ? 0.0 : 1.0);
    editor.push_back(p);
    if (i) rest.add_bond(i-1,i,1.0e-5);
  }
  GpuTopologyBondIndex topology(rest,114);
  const auto position=[&](std::size_t i)->Vec3 {return editor.at(i);};
  auto source=editor;
  for (NativeSculptBrushKind kind :
       {NativeSculptBrushKind::Clay,
        NativeSculptBrushKind::ClayStrips,
        NativeSculptBrushKind::Smooth,
        NativeSculptBrushKind::Draw,
        NativeSculptBrushKind::Nudge,
        NativeSculptBrushKind::Gravity}) {
    editor=source;
    NativeBrushDab dab;
    dab.kind=kind;
    dab.topology_generation=114;
    dab.compliance=1e-6;
    dab.dt=1.0/60.0;
    const double sign=kind==NativeSculptBrushKind::Smooth ? -1.0 : 1.0;
    const std::vector<NativeBrushTranslation> samples{
        {5,{0.0,0.3*sign,0.0}},
        {6,{0.0,0.2*sign,0.0}},
        {10,{0.0,1.0,0.0}}, // pinned, never moved
        {7,{0.0,0.0,0.0}}}; // zero factor, no target

    auto staged=prepare_cpu_native_brush_dab(rest,topology,samples,position,dab);
    assert(staged.has_solver() && staged.revision()>0);
    assert(!staged.writes().empty());
    for (const auto &edit:staged.writes()) {
      assert(edit.global_index==5 || edit.global_index==6);
      assert(std::isfinite(edit.position.x) && std::isfinite(edit.position.y));
      editor[edit.global_index]=edit.position;
    }
    assert((editor[5].y-source[5].y)*sign>0.0);
    assert(editor[10].y==source[10].y);
    assert(editor[7].y==source[7].y);
    staged.finish_editor_success();

    // Following dab must use the current edited position, not the rest pose.
    const double previous=editor[5].y;
    auto next=prepare_cpu_native_brush_dab(
        rest,topology,{{5,{0.0,0.1*sign,0.0}}},position,dab);
    assert(next.has_solver());
    for (const auto &edit:next.writes()) editor[edit.global_index]=edit.position;
    next.finish_editor_success();
    assert((editor[5].y-previous)*sign>0.0);

    // Discarded staging has NO effect on the authoritative editor mesh.
    const auto before=editor;
    {
      auto cancel=prepare_cpu_native_brush_dab(
          rest,topology,{{8,{0.0,0.4,0.0}}},position,dab);
      assert(cancel.has_solver());
    }
    for (std::size_t i=0;i<editor.size();++i) {
      assert(editor[i].x==before[i].x && editor[i].y==before[i].y);
    }
    assert(rest.particles()[5].position.y==source[5].y);
  }

  // Artist assist is a blend of the ORIGINAL native displacement and
  // fully solved displacement, not a second pressure/mask multiplier.
  editor=source;
  for (auto kind : {NativeSculptBrushKind::Clay,NativeSculptBrushKind::ClayStrips,
                    NativeSculptBrushKind::Smooth,NativeSculptBrushKind::Draw,
                    NativeSculptBrushKind::Nudge,NativeSculptBrushKind::Gravity}) {
    NativeBrushDab settings;
    settings.kind=kind;
    settings.topology_generation=114;
    const double dy=kind==NativeSculptBrushKind::Smooth ? -0.4 : 0.4;
    const std::vector<NativeBrushTranslation> input{{5,{0,dy,0}},
                                                    {10,{0,1,0}}};
    settings.assist=0.0;
    auto native=prepare_cpu_native_brush_dab(rest,topology,input,position,settings);
    assert(native.writes().size()==1 && native.writes()[0].global_index==5);
    const Vec3 n=native.writes()[0].position;
    assert(n.x==source[5].x && n.y==source[5].y+dy);
    native.finish_editor_success();
    settings.assist=1.0;
    auto full=prepare_cpu_native_brush_dab(rest,topology,input,position,settings);
    assert(full.writes().size()==1);
    const Vec3 p=full.writes()[0].position;
    full.cancel();
    settings.assist=0.25;
    auto blend=prepare_cpu_native_brush_dab(rest,topology,input,position,settings);
    assert(blend.writes().size()==1 && blend.writes()[0].global_index==5);
    const Vec3 m=blend.writes()[0].position;
    assert(std::abs(m.x-(0.75*n.x+0.25*p.x))<1e-12);
    assert(std::abs(m.y-(0.75*n.y+0.25*p.y))<1e-12);
    assert(std::abs(m.z-(0.75*n.z+0.25*p.z))<1e-12);
    blend.finish_editor_success();
    assert(editor[5].y==source[5].y && editor[10].y==source[10].y);
  }

  // Profiles are opt-in. On a single active point between fixed neighbors,
  // following native target edge lengths should eliminate unwanted elastic
  // return toward the initial surface. This is a numerical fidelity test,
  // not a claim of enclosed-volume conservation.
  editor=source;
  for (auto kind : {NativeSculptBrushKind::Clay,NativeSculptBrushKind::ClayStrips,
                    NativeSculptBrushKind::Smooth,NativeSculptBrushKind::Draw}) {
    NativeBrushDab settings;
    settings.kind=kind;
    settings.topology_generation=114;
    const double dy=kind==NativeSculptBrushKind::Smooth ? -0.6 : 0.6;
    const std::vector<NativeBrushTranslation> input{{5,{0,dy,0}},
                                                    {10,{0,1,0}}};
    auto legacy=prepare_cpu_native_brush_dab(rest,topology,input,position,settings);
    assert(legacy.writes().size()==1);
    const Vec3 old=legacy.writes()[0].position;
    legacy.cancel();
    const auto profile=experimental_native_brush_profile(kind);
    settings.compliance=profile.target_compliance;
    settings.material=profile.material;
    auto tuned=prepare_cpu_native_brush_dab(rest,topology,input,position,settings);
    assert(tuned.writes().size()==1 && tuned.writes()[0].global_index==5);
    const Vec3 p=tuned.writes()[0].position;
    assert(std::hypot(p.x-5,p.y-dy,p.z)<std::hypot(old.x-5,old.y-dy,old.z));
    tuned.cancel();
    assert(rest.particles()[5].position.y==0.0);
  }
  // Full rest-follow matches the finished native geometry even with stiff
  // bonds, and a coincident goal is handled with positive rest length.
  NativeBrushDab follow;
  follow.topology_generation=114;
  follow.compliance=0.0;
  follow.material={0.0,1.0};
  auto exact=prepare_cpu_native_brush_dab(
      rest,topology,{{5,{0,0.6,0}}},position,follow);
  assert(exact.writes().size()==1);
  assert(std::abs(exact.writes()[0].position.y-0.6)<1e-4);
  exact.cancel();
  auto coincident=prepare_cpu_native_brush_dab(
      rest,topology,{{5,{1,0,0}}},position,follow);
  for (const auto &w : coincident.writes()) {
    assert(w.global_index==5 && std::isfinite(w.position.x));
  }
  coincident.cancel();

  // Clay V2: pressure gradient creates a tangential response on an
  // otherwise flat line, while pinned vertices remain untouched.
  {
    World clay_world;
    std::vector<Vec3> points{{0,0,0},{1,0,0},{2,0,0}};
    clay_world.add_particle(points[0]);
    clay_world.add_particle(points[1]);
    clay_world.add_particle(points[2],0.0);
    clay_world.add_bond(0,1,1e-5);
    clay_world.add_bond(1,2,1e-5);
    GpuTopologyBondIndex clay_index(clay_world,23);
    const auto at=[&](std::size_t i) {return points.at(i);};
    const std::vector<NativeBrushTranslation> input{
        {0,{0,0,0.4}}, {1,{0,0,0.2}}, {2,{0,0,1.0}}};
    NativeBrushDab settings;
    settings.kind=NativeSculptBrushKind::Clay;
    settings.topology_generation=23;
    settings.clay_material=true;
    settings.compliance=0.0;
    settings.material.bond_compliance_scale=1000.0;
    settings.clay_spread=0.0;
    auto no_spread=prepare_cpu_native_brush_dab(
        clay_world,clay_index,input,at,settings);
    assert(no_spread.writes().size()==2);
    const auto baseline=no_spread.writes()[0].position;
    no_spread.cancel();
    settings.clay_spread=1.0;
    auto physical=prepare_cpu_native_brush_dab(
        clay_world,clay_index,input,at,settings);
    assert(physical.writes().size()==2);
    const auto p=physical.writes()[0].position;
    assert(p.x<baseline.x-0.01);
    for (const auto &write : physical.writes()) {
      assert(write.global_index<2);
      assert(std::isfinite(write.position.x) && std::isfinite(write.position.z));
    }
    physical.cancel();
    settings.assist=0.5;
    auto blended=prepare_cpu_native_brush_dab(
        clay_world,clay_index,input,at,settings);
    assert(blended.writes().size()==2);
    const auto m=blended.writes()[0].position;
    assert(std::abs(m.x-0.5*p.x)<1e-12);
    assert(std::abs(m.z-0.5*(0.4+p.z))<1e-12);
    blended.cancel();
    settings.assist=0.0;
    auto native=prepare_cpu_native_brush_dab(
        clay_world,clay_index,input,at,settings);
    assert(native.writes().size()==2);
    assert(native.writes()[0].position.x==0.0);
    assert(native.writes()[0].position.z==0.4);
    native.cancel();
    settings.clay_spread=1.1;
    bool bad=false;
    try {
      (void)prepare_cpu_native_brush_dab(clay_world,clay_index,input,at,settings);
    }
    catch (const std::invalid_argument &) {bad=true;}
    assert(bad);
    assert(clay_world.particles()[2].position.x==2.0);
  }

  // V3: test the brush footprint on two mesh densities, rather than
  // asserting only that adjacent vertices moved by a nonzero amount.
  for (const int segments : {8,64}) {
    World surface;
    std::vector<Vec3> points;
    std::vector<NativeBrushTranslation> input;
    for (int i=0;i<=segments;++i) {
      const double x=-1.0+2.0*double(i)/segments;
      points.push_back({x,0.0,0.0});
      surface.add_particle(points.back());
      if (i) surface.add_bond(i-1,i,1e-5);
      input.push_back({std::size_t(i),{0,0,0.2}});
    }
    GpuTopologyBondIndex index(surface,29);
    const auto at=[&](std::size_t i) {return points.at(i);};
    NativeBrushDab settings;
    settings.kind=NativeSculptBrushKind::Clay;
    settings.topology_generation=29;
    settings.clay_material=true;
    settings.clay_radius=1.0;
    settings.clay_center={0,0,0};
    settings.clay_spread=1.0;
    settings.compliance=0.0;
    settings.material.bond_compliance_scale=1000.0;
    settings.clay_behavior=2;
    auto compress=prepare_cpu_native_brush_dab(surface,index,input,at,settings);
    std::vector<Vec3> compressed=points;
    for (const auto &w : compress.writes()) compressed.at(w.global_index)=w.position;
    assert(compressed[segments/2].z<-0.05);
    assert(compressed[segments].z>0.05);
    double sum=0.0;
    for (auto p : compressed) sum+=p.z;
    assert(std::abs(sum/double(points.size()))<0.01);
    compress.cancel();
    settings.clay_behavior=1;
    auto deposit=prepare_cpu_native_brush_dab(surface,index,input,at,settings);
    std::vector<Vec3> deposited=points;
    for (const auto &w : deposit.writes()) deposited.at(w.global_index)=w.position;
    assert(deposited[segments].z>deposited[segments/2].z);
    assert(deposited[segments/2].z>0.0);
    deposit.cancel();
    settings.clay_behavior=3;
    settings.clay_stroke_delta={0.2,0,0};
    auto flow=prepare_cpu_native_brush_dab(surface,index,input,at,settings);
    std::vector<Vec3> flowed=points;
    for (const auto &w : flow.writes()) flowed.at(w.global_index)=w.position;
    assert(flowed[segments/2].x>0.15);
    assert(flowed[segments/2].z<0.1);
    flow.cancel();
    settings.assist=0.0;
    auto native=prepare_cpu_native_brush_dab(surface,index,input,at,settings);
    for (const auto &w : native.writes()) {
      assert(w.position.x==points[w.global_index].x);
      assert(w.position.z==0.2);
    }
    native.cancel();
  }

  auto cfg=NativeBrushDab{};
  cfg.topology_generation=114;
  auto none=prepare_cpu_native_brush_dab(rest,topology,{},position,cfg);
  assert(!none.has_solver());
  none=prepare_cpu_native_brush_dab(
      rest,topology,{{10,{0.0,0.3,0.0}}},position,cfg);
  assert(!none.has_solver());
  none=prepare_cpu_native_brush_dab(
      rest,topology,{{7,{0.0,0.0,0.0}}},position,cfg);
  assert(!none.has_solver());
  const auto rejects=[&](std::vector<NativeBrushTranslation> samples,NativeBrushDab dab) {
    bool failed=false;
    try {(void)prepare_cpu_native_brush_dab(rest,topology,samples,position,dab);}
    catch (const std::exception &) {failed=true;}
    assert(failed);
  };
  rejects({{5,{0,0.1,0}},{5,{0,0.2,0}}},cfg);
  rejects({{21,{0,0.1,0}}},cfg);
  rejects({{5,{0,std::numeric_limits<double>::infinity(),0}}},cfg);
  rejects({{5,{0,std::numeric_limits<double>::quiet_NaN(),0}}},cfg);
  cfg.topology_generation=115;
  rejects({{5,{0,0.1,0}}},cfg);
  cfg.topology_generation=114;
  cfg.compliance=-1;
  rejects({{5,{0,0.1,0}}},cfg);
  cfg.compliance=1e-6;
  for (double assist : {-0.1,1.1,std::numeric_limits<double>::quiet_NaN()}) {
    cfg.assist=assist;
    rejects({{5,{0,0.1,0}}},cfg);
  }
  cfg.assist=1.0;
  for (auto material : {NativeMaterialResponse{-1,0},
                        NativeMaterialResponse{1,1.1},
                        NativeMaterialResponse{1,-0.1},
                        NativeMaterialResponse{std::numeric_limits<double>::infinity(),0}}) {
    cfg.material=material;
    rejects({{5,{0,0.1,0}}},cfg);
  }
  cfg.material={};
  cfg.kind=static_cast<NativeSculptBrushKind>(999);
  rejects({{5,{0,0.1,0}}},cfg);

  bool invalid_current=false;
  try {
    auto bad=[](std::size_t)->Vec3 {return {NAN,0,0};};
    (void)prepare_cpu_native_brush_dab(
        rest,topology,{{5,{0,0.1,0}}},bad,NativeBrushDab{
            NativeSculptBrushKind::Clay,1e-6,1.0/60.0,2,5,114});
  } catch (const std::invalid_argument &) {invalid_current=true;}
  assert(invalid_current);
  return 0;
}
