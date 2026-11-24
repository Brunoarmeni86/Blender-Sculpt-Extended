/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <numeric>

using namespace sculpt_physics;
struct Profile {
  double absolute=0,net=0,center=0,moment_x=0;
  std::vector<Vec3> motion;
};
static Profile profile(int n,ClaySurfaceAction action,double phase=0)
{
  std::vector<ClaySurfaceVertex> vertices;
  std::vector<ClaySurfaceTriangle> tris;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
    const double px=2.0*x/(n-1)-1+phase,py=2.0*y/(n-1)-1;
    const double rr=(px*px+py*py)/(.8*.8);
    const double w=rr<1?std::pow(1-rr,2):0;
    vertices.push_back({{px,py,.12*std::exp(-22*(px*px+py*py))},w});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x) {
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tris.push_back({a,b,d});tris.push_back({a,d,c});
  }
  ClaySurfaceDab dab;
  dab.action=action;dab.radius=.8;dab.dose=.20;dab.intensity=.9;
  dab.mobility=.9;dab.direction={1,0,0};dab.friction=.85;
  dab.viscosity=.2;dab.cohesion=.35;dab.stiffness=.25;
  dab.yield_slope=.05;dab.density_independent=true;dab.smooth_proxy=true;
  const auto step=solve_material_transport_v21(vertices,tris,dab,4);
  assert(step.surface.transport_nodes>0);
  Profile out;
  out.motion=step.surface.displacement;
  for(std::size_t i=0;i<vertices.size();++i) {
    const auto &d=out.motion[i];
    const double a=step.surface.projected_area[i];
    assert(d.x==0 && d.y==0 && std::isfinite(d.z));
    if(vertices[i].influence==0)assert(d.z==0);
    out.absolute+=a*std::abs(d.z);out.net+=a*d.z;
    out.moment_x+=a*d.z*vertices[i].position.x;
  }
  out.center=out.motion[(n/2)*n+n/2].z;
  assert(std::abs(out.net)<1e-9);
  assert(out.absolute>1e-8);
  return out;
}
int main()
{
  for(auto a:{ClaySurfaceAction::Push,ClaySurfaceAction::Gather,
              ClaySurfaceAction::Smear,ClaySurfaceAction::Melt}) {
    const auto coarse=profile(33,a),fine=profile(129,a),shift=profile(129,a,.004);
    std::cout<<"action="<<int(a)<<" l1_33="<<coarse.absolute
             <<" l1_129="<<fine.absolute<<" phase_l1="<<shift.absolute
             <<" center="<<fine.center<<" moment_x="<<fine.moment_x<<std::endl;
    assert(fine.absolute/coarse.absolute>.65 && fine.absolute/coarse.absolute<1.55);
    assert(shift.absolute/fine.absolute>.8 && shift.absolute/fine.absolute<1.2);
    if(a==ClaySurfaceAction::Gather) assert(fine.center>0);
    if(a==ClaySurfaceAction::Melt) assert(fine.center<0);
  }
  const auto flow=profile(65,ClaySurfaceAction::Push);
  const auto smear=profile(65,ClaySurfaceAction::Smear);
  const auto melt=profile(65,ClaySurfaceAction::Melt);
  assert(flow.moment_x>smear.moment_x);
  assert(std::abs(melt.moment_x)<std::abs(flow.moment_x)*.1);
  double difference=0;
  for(std::size_t i=0;i<flow.motion.size();++i)
    difference+=std::abs(flow.motion[i].z-smear.motion[i].z);
  assert(difference>1e-6);
}
