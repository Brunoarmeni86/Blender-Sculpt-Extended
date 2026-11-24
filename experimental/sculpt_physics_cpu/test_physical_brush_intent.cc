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
