/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physical_brush_intent.hh"

#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>

using namespace sculpt_physics;

int main()
{
  PhysicalBrushIntentDab dab;
  dab.dose=0.5;
  dab.spread=1.0;
  // Deliberately unordered IDs; include a pinned point and zero envelope.
  std::vector<PhysicalBrushIntentSample> samples{
      {4,{0.8,0,0},0.5,2.0,true},
      {0,{0,0,0},1.0,1.0,true},
      {1,{0.3,0,0},1.0,1.0,false},
      {2,{0.4,0,0},0.0,1.0,true}};
  auto deposit=plan_physical_brush_intent(samples,dab);
  assert(deposit.size()==2 && deposit[0].global_index==0 && deposit[1].global_index==4);
  assert(deposit[0].translation.z>0 && deposit[1].translation.z>0);
  assert(std::abs(deposit[0].translation.z+2*deposit[1].translation.z-0.1)<1e-12);
  dab.action=PhysicalBrushAction::Compress;
  auto compress=plan_physical_brush_intent(samples,dab);
  assert(compress.size()==2);
  assert(compress[0].translation.z<0 && compress[1].translation.z>0);
  assert(std::abs(compress[0].translation.z+2*compress[1].translation.z)<1e-12);
  // Repeated inputs remain effective even when there is no native deformation.
  // This verifies the target planner, NOT persistent plastic state.
  double cumulative=0.0;
  for (int i=0;i<10;++i) {
    auto repeated=plan_physical_brush_intent(samples,dab);
    cumulative+=repeated[0].translation.z;
    samples[1].position.z+=repeated[0].translation.z;
    samples[0].position.z+=repeated[1].translation.z;
  }
  assert(std::abs(cumulative-10*compress[0].translation.z)<1e-12);
  // Dose subdivision on a fixed footprint preserves intended action budget.
  dab.dose=0.25;
  auto half=plan_physical_brush_intent(samples,dab);
  assert(std::abs(2*half[0].translation.z-compress[0].translation.z)<1e-12);
  dab.action=PhysicalBrushAction::Flow;
  assert(plan_physical_brush_intent(samples,dab).empty());
  dab.stroke_direction={1,0,1};
  auto flow=plan_physical_brush_intent(samples,dab);
  assert(flow.size()==2 && flow[0].translation.x>0 && flow[0].translation.z==0);
  // Native cube footprint can legitimately contain projected r > 1.
  const std::vector<PhysicalBrushIntentSample> cube{{9,{0.9,0.9,0},1,1,true}};
  assert(plan_physical_brush_intent(cube,dab).empty());
  dab.limit_to_projected_disk=false;
  assert(plan_physical_brush_intent(cube,dab).size()==1);
  dab.limit_to_projected_disk=true;
  dab.intensity=0;
  assert(plan_physical_brush_intent(samples,dab).empty());
  dab.intensity=1;
  dab.dose=0;
  assert(plan_physical_brush_intent(samples,dab).empty());
  dab.dose=0.5;
  // V18-B: pressure actions must work without a gesture. No forced rim
  // for Compact; Press does not pull points already below the tool.
  dab.stroke_direction={};
  const std::vector<PhysicalBrushIntentSample> pressure_samples{
      {0,{0,0,0},1,1,true,{0,0,1}},
      {1,{0.5,0,0},1,1,true,{0.6,0,0.8}},
      {2,{-0.5,0,-0.5},1,1,true,{0,0,1}},
      {3,{0,0.5,0},0,1,true,{0,0,1}},
      {4,{0,-0.5,0},1,1,false,{0,0,1}}};
  dab.action=PhysicalBrushAction::Compact;
  const auto compact=plan_physical_brush_intent(pressure_samples,dab);
  assert(compact.size()==3 && compact[0].translation.z<0);
  assert(compact[1].translation.z<0 && compact[2].translation.z<0);
  dab.action=PhysicalBrushAction::Press;
  const auto press=plan_physical_brush_intent(pressure_samples,dab);
  assert(press.size()==2 && press[0].translation.z<0);
  dab.action=PhysicalBrushAction::PressureFlow;
  const auto radial=plan_physical_brush_intent(pressure_samples,dab);
  assert(radial.size()==2 && radial[0].global_index==1);
  assert(radial[0].translation.x>0);
  assert(std::abs(radial[0].translation.x*0.6+radial[0].translation.z*0.8)<1e-12);
  assert(radial[1].translation.x<0);
  dab.radial_sign=-1;
  const auto inward=plan_physical_brush_intent(pressure_samples,dab);
  assert(inward[0].translation.x<0);
  dab.radial_sign=1;
  dab.action=PhysicalBrushAction::HybridFlow;
  assert(plan_physical_brush_intent(pressure_samples,dab).size()==2);
  dab.stroke_direction={1,0,0};
  const auto hybrid=plan_physical_brush_intent(pressure_samples,dab);
  assert(hybrid.size()==3 && hybrid[0].translation.x>0);
  dab.dose=0.0;
  assert(plan_physical_brush_intent(pressure_samples,dab).empty());
  dab.dose=0.5;
  auto duplicate=samples;
  duplicate.push_back(samples[0]);
  bool rejected=false;
  try { (void)plan_physical_brush_intent(duplicate,dab); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  samples[0].envelope=std::numeric_limits<double>::quiet_NaN();
  rejected=false;
  try { (void)plan_physical_brush_intent(samples,dab); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
}
