/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "clay_material_layer.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <numeric>

using namespace sculpt_physics;

static double sum(const std::vector<double> &v)
{
  return std::accumulate(v.begin(),v.end(),0.0);
}
int main()
{
  std::vector<ClayLayerCell> cells(5);
  for (std::size_t i=0;i<cells.size();++i) {
    cells[i].position={double(i)-2,0,0};
  }
  cells[1].area=2;
  cells[3].influence=0;
  cells[4].writable=false;
  const std::vector<ClayLayerEdge> edges{{0,1},{1,2},{2,3},{3,4}};
  ClayLayerDab dab;
  dab.radius=5;
  dab.amount=4;
  const auto add=stage_clay_layer(cells,edges,dab);
  assert(std::abs(sum(add.volume)-4)<1e-12);
  assert(std::abs(add.added_volume-4)<1e-12);
  assert(add.volume[0]==1 && add.volume[1]==2 && add.volume[2]==1);
  assert(add.volume[3]==0 && add.volume[4]==0);
  assert(add.displacement[0].z==add.displacement[1].z);
  assert(cells[0].volume==0); // pure staging
  for (std::size_t i=0;i<cells.size();++i) cells[i].volume=add.volume[i];
  dab.action=ClayLayerAction::Push;
  dab.direction={1,0,0};dab.amount=1;dab.mobility=1;
  const auto push=stage_clay_layer(cells,edges,dab);
  assert(std::abs(sum(push.volume)-4)<1e-12);
  assert(push.volume[0]<cells[0].volume && push.volume[2]>cells[2].volume);
  assert(push.volume[3]==0 && push.volume[4]==0 && push.added_volume==0);
  assert(push.displacement[0].z<0 && push.displacement[2].z>0);
  auto reverse=edges;
  std::reverse(reverse.begin(),reverse.end());
  assert(stage_clay_layer(cells,reverse,dab).volume==push.volume);
  // Multiple overlapping deposition dabs use explicit volume, not point count.
  dab.action=ClayLayerAction::Deposit;dab.amount=0.5;
  for (int k=0;k<40;++k) {
    const auto step=stage_clay_layer(cells,edges,dab);
    for (std::size_t i=0;i<cells.size();++i) cells[i].volume=step.volume[i];
  }
  double total=0;
  for (const auto &c:cells) total+=c.volume;
  assert(std::abs(total-24)<1e-10);
  dab.action=ClayLayerAction::Push;dab.amount=0.4;
  for (int k=0;k<100;++k) {
    const auto step=stage_clay_layer(cells,edges,dab);
    assert(std::abs(sum(step.volume)-24)<1e-9);
    for (std::size_t i=0;i<cells.size();++i) {
      assert(step.volume[i]>=0);
      cells[i].volume=step.volume[i];
    }
  }
  assert(cells[2].volume>23);
  dab.action=ClayLayerAction::Spread;dab.amount=1;
  const auto spread=stage_clay_layer(cells,edges,dab);
  assert(spread.volume[1]>cells[1].volume && spread.volume[2]<cells[2].volume);
  assert(std::abs(sum(spread.volume)-24)<1e-9);
  dab.action=ClayLayerAction::Gather;dab.center={-1,0,0};
  const auto gather=stage_clay_layer(cells,edges,dab);
  assert(gather.volume[1]>cells[1].volume);
  assert(std::abs(sum(gather.volume)-24)<1e-9);
  // Equal total area split into more samples: same thickness and budget.
  std::vector<ClayLayerCell> coarse(1),fine(4);
  coarse[0].area=4;
  dab.action=ClayLayerAction::Deposit;dab.amount=2;
  auto c=stage_clay_layer(coarse,{},dab);
  auto f=stage_clay_layer(fine,{},dab);
  assert(sum(c.volume)==sum(f.volume));
  for (const auto &d:f.displacement) assert(d.z==c.displacement[0].z);
  fine[0].writable=false;
  for (auto &v:fine) v.influence=0;
  assert(stage_clay_layer(fine,{},dab).added_volume==0);
  bool rejected=false;
  try {stage_clay_layer(cells,{{0,1},{1,0}},dab);}
  catch (const std::invalid_argument &) {rejected=true;}
  assert(rejected);
}
