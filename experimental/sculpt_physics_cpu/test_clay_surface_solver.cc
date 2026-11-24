/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "clay_surface_solver.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <numeric>

using namespace sculpt_physics;

int main()
{
  constexpr int side=5;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
    const double px=0.2*(x-2),py=0.2*(y-2);
    v.push_back({{px,py,0.08*std::exp(-20*(px*px+py*py))},
                 x>0 && x<side-1 && y>0 && y<side-1 ? 1.0:0.0});
  }
  for(int y=0;y<side-1;++y) for(int x=0;x<side-1;++x) {
    const auto a=std::size_t(y*side+x),b=a+1,c=a+side,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  ClaySurfaceDab dab;
  dab.radius=0.8;dab.dose=0.12;dab.mobility=0.8;
  auto deposit=solve_clay_surface(v,t,dab);
  assert(deposit.active==9 && deposit.signed_swept_volume>0);
  double integrated=0;
  for(const auto &tri:t) {
    const auto a=v[tri[0]].position,b=v[tri[1]].position,c=v[tri[2]].position;
    const double projected=0.5*((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x));
    integrated+=projected*(deposit.displacement[tri[0]].z+
                           deposit.displacement[tri[1]].z+
                           deposit.displacement[tri[2]].z)/3;
  }
  assert(std::abs(integrated-deposit.signed_swept_volume)<1e-12);
  assert(deposit.displacement[12].z>0 && deposit.displacement[0].z==0);
  dab.action=ClaySurfaceAction::Push;dab.direction={1,0,0};
  auto push=solve_clay_surface(v,t,dab);
  assert(push.transported_volume>0);
  assert(std::abs(push.signed_swept_volume)<1e-12);
  assert(push.displacement[11].z<0 && push.displacement[13].z>0);
  assert(push.displacement[0].z==0);
  const auto initial=v;
  // Repeated accepted updates conserve projected material budget.
  for(int k=0;k<50;++k) {
    auto step=solve_clay_surface(v,t,dab);
    assert(std::abs(step.signed_swept_volume)<1e-10);
    for(std::size_t i=0;i<v.size();++i) {
      assert(step.displacement[i].x==0 && step.displacement[i].y==0);
      if(v[i].influence==0) assert(step.displacement[i].z==0);
      v[i].position+=step.displacement[i];
    }
  }
  v=initial;
  // Gather can build a ridge even without directional input.
  dab.action=ClaySurfaceAction::Gather;dab.direction={};
  auto gather=solve_clay_surface(v,t,dab);
  assert(gather.transported_volume>0 && gather.displacement[12].z>0);
  assert(std::abs(gather.signed_swept_volume)<1e-12);
  dab.action=ClaySurfaceAction::Spread;
  auto spread=solve_clay_surface(v,t,dab);
  assert(std::abs(spread.signed_swept_volume)<1e-12);
  // A disconnected, oppositely wound sheet must never be modified.
  const auto first=v.size();
  v.push_back({{0,0,-0.05},1});
  v.push_back({{0.1,0,-0.05},1});
  v.push_back({{0,0.1,-0.05},1});
  t.push_back({first,first+2,first+1});
  dab.action=ClaySurfaceAction::Buildup;
  auto isolated=solve_clay_surface(v,t,dab);
  for(std::size_t i=first;i<v.size();++i) assert(isolated.displacement[i].z==0);
  // Uniformly flat chart stays stationary under yield-limited spreading.
  for(auto &p:v) p.position.z=0;
  dab.action=ClaySurfaceAction::Spread;
  assert(solve_clay_surface(v,t,dab).transported_volume==0);
  dab.dose=0;
  assert(solve_clay_surface(v,t,dab).transported_volume==0);
  bool rejected=false;
  try { solve_clay_surface(v,{{0,0,1}},dab); }
  catch(const std::invalid_argument &) {rejected=true;}
  assert(rejected);
}
