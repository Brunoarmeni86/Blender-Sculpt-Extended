/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#include "material_profiles_v25.hh"
#ifdef NDEBUG
# undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
using namespace sculpt_physics;

static ClaySurfaceResult trial(int n,ClaySurfaceAction action,double angle=0,
                              double sign=1,double plane=-.05,bool raised=false,
                              std::string_view profile={})
{
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  const Vec3 dir{std::cos(angle),std::sin(angle),0};
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.0*x/(n-1)-1,py=2.0*y/(n-1)-1;
    const double rr=(px*px+py*py)/(.8*.8);
    const double w=rr<1?std::pow(1-rr,2):0;
    v.push_back({{px,py,raised?.06*std::exp(-20*(px*px+py*py)):0},w});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.action=action;dab.radius=.8;dab.dose=.15;dab.intensity=1;
  dab.direction=dir;dab.mobility=.85;dab.friction=.85;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;dab.tool_v25=true;
  dab.has_tool_plane=true;dab.tool_plane_height=plane;dab.sign=sign;
  if(!profile.empty()){
    const auto p=surface_profile_v25(profile);
    dab.friction=p.friction;dab.viscosity=p.viscosity;dab.cohesion=p.cohesion;
    dab.stiffness=p.stiffness;dab.plasticity=p.plasticity;
    dab.volume_preservation=p.volume_preservation;dab.fusion=p.fusion;
    dab.detail_retention=p.detail_retention;
  }
  const auto out=solve_material_transport_v21(v,t,dab,4).surface;
  double net=0,abs=0,moment=0,peak=0;
  for(std::size_t i=0;i<v.size();++i){
    const auto d=out.displacement[i];
    assert(std::isfinite(d.z)&&d.x==0&&d.y==0);
    if(v[i].influence==0)assert(d.z==0);
    if(action==ClaySurfaceAction::Scrape)assert(d.z*sign<=1e-12);
    net+=d.z*out.projected_area[i];abs+=std::abs(d.z)*out.projected_area[i];
    moment+=(v[i].position.x*dir.x+v[i].position.y*dir.y)*d.z*out.projected_area[i];
    peak=std::max(peak,std::abs(d.z));
  }
  std::cout<<"action="<<int(action)<<" n="<<n<<" angle="<<angle<<" l1="<<abs
           <<" moment="<<moment<<" net="<<net<<" peak="<<peak<<std::endl;
  assert(std::abs(net-out.signed_swept_volume)<1e-9);
  if(action==ClaySurfaceAction::Push){
    assert(abs>1e-6 && sign*moment>1e-7 && std::abs(net)<1e-9);
  }
  if(action==ClaySurfaceAction::Press)assert(std::abs(net)<1e-9);
  return out;
}
int main(){
  for(int n:{33,65,129}){
    trial(n,ClaySurfaceAction::Push);
    trial(n,ClaySurfaceAction::Push,.7853981633974483);
  }
  trial(65,ClaySurfaceAction::Push,0,-1);
  const auto cut=trial(65,ClaySurfaceAction::Scrape);
  assert(cut.signed_swept_volume< -1e-6);
  const auto clear=trial(65,ClaySurfaceAction::Scrape,0,1,.10,true);
  assert(std::abs(clear.signed_swept_volume)<1e-12);
  const auto soft=trial(65,ClaySurfaceAction::Push,0,1,-.05,false,"SOFT_CLAY");
  const auto hard=trial(65,ClaySurfaceAction::Push,0,1,-.05,false,"HARD_CLAY");
  assert(soft.transported_volume>hard.transported_volume);
  const auto press=trial(65,ClaySurfaceAction::Press);
  assert(press.transported_volume>1e-7);
}
