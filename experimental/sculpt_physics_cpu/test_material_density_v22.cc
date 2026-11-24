/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

using namespace sculpt_physics;
struct Metrics {
  double l1=0,net=0,center=0,peak=0,transport=0;
  std::size_t nodes=0;
};
static Metrics measure(int n,ClaySurfaceAction action,bool v22)
{
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
    const double px=2.0*x/(n-1)-1,py=2.0*y/(n-1)-1;
    const double r=std::hypot(px,py)/.8;
    const double w=r<1?std::pow(1-r*r,2):0;
    v.push_back({{px,py,.04*std::exp(-12*(px*px+py*py))},w});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x) {
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    if((x+y)%2){t.push_back({a,b,c});t.push_back({b,d,c});}
    else{t.push_back({a,b,d});t.push_back({a,d,c});}
  }
  ClaySurfaceDab dab;
  dab.action=action;dab.radius=.8;dab.dose=.2;dab.intensity=.9;
  dab.mobility=.85;dab.direction={1,0,0};dab.friction=.85;
  dab.viscosity=.2;dab.cohesion=.35;dab.stiffness=.25;
  dab.volume_preservation=.75;dab.yield_slope=.05;
  dab.density_independent=v22;
  const auto result=solve_material_transport_v21(v,t,dab,4).surface;
  Metrics m;
  m.nodes=result.transport_nodes;m.transport=result.transported_volume;
  for(std::size_t i=0;i<v.size();++i) {
    const auto d=result.displacement[i];
    assert(std::isfinite(d.z) && d.x==0 && d.y==0);
    if(v[i].influence==0)assert(d.z==0);
    m.l1+=std::abs(d.z)*result.projected_area[i];
    m.net+=d.z*result.projected_area[i];
    m.peak=std::max(m.peak,std::abs(d.z));
  }
  m.center=result.displacement[(n/2)*n+n/2].z;
  assert(std::abs(m.net-result.signed_swept_volume)<1e-9);
  return m;
}
int main()
{
  for(auto action:{ClaySurfaceAction::FusionDeposit,ClaySurfaceAction::Push,
                   ClaySurfaceAction::Compress,ClaySurfaceAction::Compact,
                   ClaySurfaceAction::Press,ClaySurfaceAction::PressureFlow,
                   ClaySurfaceAction::HybridFlow}) {
    double ref=0;
    for(int n:{17,33,65,129}) {
      const auto old=measure(n,action,false),now=measure(n,action,true);
      std::cout<<"action="<<int(action)<<" resolution="<<n
               <<" v21_l1="<<old.l1<<" v22_l1="<<now.l1
               <<" v22_center="<<now.center<<" peak="<<now.peak
               <<" net="<<now.net<<" proxy_nodes="<<now.nodes<<std::endl;
      assert(now.nodes>0 && now.nodes<400);
      assert(now.l1>1e-9);
      if(action==ClaySurfaceAction::FusionDeposit)assert(now.net>0);
      else if(action==ClaySurfaceAction::Compact)assert(now.net<0);
      else assert(std::abs(now.net)<1e-9);
      if(n==33)ref=now.l1;
      if(n>33) {
        // Broad regression bound pending calibration against native
        // brush results; report exact metrics for tighter later gates.
        assert(now.l1/ref>.4 && now.l1/ref<2.5);
      }
    }
  }
}
