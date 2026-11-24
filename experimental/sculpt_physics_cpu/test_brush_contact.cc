/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "brush_contact.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>

using namespace sculpt_physics;

int main()
{
  std::vector<PhysicalBrushIntentSample> samples{
      {0,{0,0,0},1,1,true,{0,0,1}},
      {1,{0.4,0,0},0.5,1,true,{0,0,1}},
      {2,{0,0.3,0},0,1,true,{0,0,1}},
      {3,{0,-0.3,0},1,1,false,{0,0,1}},
      {4,{0.1,0,-0.1},1,1,true,{0,0,-1}}};
  PhysicalBrushIntentDab dab;
  dab.radius=1;dab.dose=0.5;dab.intensity=1;dab.spread=1;
  dab.stroke_direction={1,0,0};
  dab.action=PhysicalBrushAction::Compress;
  BrushContactHistory history;
  for(const auto &s:samples) history.emplace(s.global_index,BrushContactReference{s.position,{0,0,1}});
  for(int k=0;k<40;++k) {
    auto goals=plan_drag_contact(samples,dab,history);
    for(const auto &g:goals) {
      assert(g.global_index<2);
      assert(g.translation.z<=0 && g.translation.x>=0);
      samples[g.global_index].position+=g.translation;
    }
  }
  assert(std::abs(samples[0].position.z+0.12)<1e-12);
  assert(std::abs(samples[1].position.z+0.12)<1e-12);
  assert(plan_drag_contact(samples,dab,history).empty());
  // Another stroke deliberately starts from the current surface.
  assert(!plan_drag_contact(samples,dab,{}).empty());
  dab.action=PhysicalBrushAction::PressureFlow;
  auto moving=plan_drag_contact(samples,dab,{});
  assert(moving.size()==2 && moving[0].translation.x>0);
  dab.stroke_direction={};
  auto stationary=plan_drag_contact(samples,dab,{});
  assert(stationary.size()==2);
  assert(std::abs(stationary[0].translation.x)<std::abs(moving[0].translation.x));
  dab.action=PhysicalBrushAction::Deposit;
  auto baseline=plan_physical_brush_intent(samples,dab);
  auto deposit=plan_drag_contact(samples,dab,{});
  assert(baseline.size()==deposit.size());
  for(std::size_t i=0;i<deposit.size();++i)
    assert(baseline[i].translation.z==deposit[i].translation.z);
  dab.intensity=0;
  assert(plan_drag_contact(samples,dab,{}).empty());

  ContactTriangle tri{{Vec3{0,0,0},Vec3{1,0,0},Vec3{0,1,0}},
                      {Vec3{},Vec3{-3,0,0},Vec3{0,-2,0}}};
  const double scale=contact_triangle_scale(tri);
  assert(scale>0 && scale<1);
  for(int i=0;i<=100;++i) {
    const double t=scale*i/100.0;
    assert((1-3*t)*(1-2*t)>=0.35-1e-10);
  }
  tri.motion={Vec3{2,3,4},Vec3{2,3,4},Vec3{2,3,4}};
  assert(contact_triangle_scale(tri)==1);
  tri.motion={Vec3{},Vec3{0,0,0.5},Vec3{0,0,-0.5}};
  assert(contact_triangle_scale(tri)==1);
  tri.position[2]=tri.position[1];
  assert(contact_triangle_scale(tri)==0);
}
