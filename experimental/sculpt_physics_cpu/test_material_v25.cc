#include "stroke_context_v27.hh"
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
static void overlapping_v26(ClaySurfaceAction action)
{
  constexpr int n=65;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.0*x/(n-1)-1,py=2.0*y/(n-1)-1;
    const bool flat=action==ClaySurfaceAction::Press || action==ClaySurfaceAction::Scrape || action==ClaySurfaceAction::Gather;
    v.push_back({{px,py,flat?0:.025*std::sin(12*px)*std::cos(8*py)},0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.action=action;dab.radius=.4;dab.dose=.15;dab.intensity=1;
  dab.direction={1,0,0};dab.mobility=.85;dab.friction=.85;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.has_tool_plane=true;
  int moving=0;
  for(int k=0;k<9;++k){
    dab.center={-.24+.06*k,0,0};
    dab.tool_plane_height=-(action==ClaySurfaceAction::Press?progressive_press_depth_v26(.07,(k+1)*.15):.07)*dab.radius;
    for(auto &p:v){
      const double x=p.position.x-dab.center.x,y=p.position.y;
      const double r2=(x*x+y*y)/(dab.radius*dab.radius);
      p.influence=r2<1 && p.position.y>-.25?std::pow(1-r2,2):0;
    }
    dab.fast_v26b=false;
    const auto full=solve_material_transport_v21(v,t,dab,4,true,false);
    dab.fast_v26b=true;
    const auto fast=solve_material_transport_v21(v,t,dab,4,false);
    dab.continuous_v26b=true;
    const auto continuous=solve_material_transport_v21(v,t,dab,4,false);
    dab.continuous_v26b=false;
    double l1=0;
    for(std::size_t i=0;i<v.size();++i){
      const auto d=fast.surface.displacement[i];
      assert(std::isfinite(d.z));
      assert(d.z==full.surface.displacement[i].z);
      assert(std::isfinite(continuous.surface.displacement[i].z));
      if(v[i].influence==0){assert(d.z==0);assert(continuous.surface.displacement[i].z==0);}
      if(action==ClaySurfaceAction::Scrape)assert(d.z<=1e-12);
      l1+=std::abs(d.z);v[i].position+=d;
    }
    if(l1>1e-7)++moving;
    if(action!=ClaySurfaceAction::Scrape && action!=ClaySurfaceAction::Buildup &&
       action!=ClaySurfaceAction::FusionDeposit && action!=ClaySurfaceAction::Compact)
      assert(std::abs(fast.surface.signed_swept_volume)<1e-8);
  }
  std::cout<<"V26 overlap action="<<int(action)<<" moving_dabs="<<moving<<std::endl;
  if(action==ClaySurfaceAction::Press || action==ClaySurfaceAction::Scrape || action==ClaySurfaceAction::Gather)
    assert(moving>=3);
}
static void stroke_context_v27_tests(){
  StrokeContextV27 a,b;
  a=stage_stroke_v27(a,{0,0,0},{0,0,1},1,1).next;
  b=a;
  const auto whole=stage_stroke_v27(a,{.8,0,0},{0,0,1},1,1);
  double dose=0;
  for(int i=1;i<=8;++i){
    const auto part=stage_stroke_v27(b,{.1*i,0,0},{0,0,1},1,1);
    dose+=part.dose;b=part.next;
  }
  assert(std::abs(whole.dose-dose)<1e-12);
  assert(std::abs(whole.next.path_length-b.path_length)<1e-12);
  assert(a.samples==1); // staging does not mutate committed history
  const auto still=stage_stroke_v27(b,b.center,{0,0,1},1,1);
  assert(still.dose==0);
  assert(stage_stroke_v27(b,{3,0,0},{0,0,1},1,1).gap);
  assert(std::abs(swept_distance_squared_v27({.5,.2,0},{0,0,0},{1,0,0})-.04)<1e-12);
  assert(swept_distance_squared_v27({2,0,0},{0,0,0},{1,0,0})==1);
  assert(swept_distance_squared_v27({1,0,0},{0,0,0},{0,0,0})==1);
}
static void contact_v27b_tests()
{
  StrokeContextV27 before;
  auto first=stage_stroke_v27(before,{0,0,0},{0,0,1},1,1);
  assert(track_tool_plane_v27(first,before,{}));
  before=first.next;
  auto next=stage_stroke_v27(before,{.1,0,-.12},{0,0,1},1,1);
  std::vector<PhysicalBrushIntentSample> samples{
      {0,{.55,0,0},1,1,true,{0,0,1}},
      {1,{.1,0,-.12},1,1,true,{0,0,1}},
      {2,{.65,0,-.4},1,1,false,{0,0,1}}};
  assert(track_tool_plane_v27(next,before,samples));
  assert(std::abs(next.next.tool_reference.z)<1e-12);
  assert(before.center.z==0);
  const auto old=next.next;
  auto unsupported=stage_stroke_v27(old,{.2,0,-.18},{0,0,1},1,1);
  assert(!track_tool_plane_v27(unsupported,old,{}));
  assert(std::abs(unsupported.next.tool_reference.z-old.tool_reference.z)<1e-12);
  for(const auto action:{ClaySurfaceAction::Scrape,ClaySurfaceAction::Press,ClaySurfaceAction::Compact,ClaySurfaceAction::Melt}){
    constexpr int n=33;
    std::vector<ClaySurfaceVertex> v;
    std::vector<ClaySurfaceTriangle> tri;
    for(int y=0;y<n;++y)for(int x=0;x<n;++x){
      const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
      v.push_back({{px,py,.03*std::cos(5*px)},0});
    }
    for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
      const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
      tri.push_back({a,b,d});tri.push_back({a,d,c});
    }
    ClaySurfaceDab dab;dab.action=action;dab.radius=.5;dab.dose=.12;dab.intensity=1;
    dab.direction={1,0,0};dab.density_independent=true;dab.smooth_proxy=true;
    dab.shape_transport=true;dab.tool_v25=true;dab.tool_v26=true;dab.fast_v26b=true;
    dab.contact_v27b=true;dab.has_tool_plane=true;dab.volume_preservation=.75;
    int moving=0;
    for(int k=0;k<8;++k){
      const double x=-.28+.06*k;
      dab.center={x,0,.03*std::cos(5*x)};
      dab.tool_plane_height=-.07*dab.radius;
      for(auto &p:v){
        const double dx=p.position.x-x,dy=p.position.y;
        const double r2=(dx*dx+dy*dy)/(dab.radius*dab.radius);
        p.influence=r2<1&&dy>-.30?std::pow(1-r2,2):0;
      }
      const auto out=solve_material_transport_v21(v,tri,dab,4,false).surface;
      double l1=0;
      for(std::size_t i=0;i<v.size();++i){
        const auto delta=out.displacement[i];
        assert(std::isfinite(delta.z));
        if(v[i].influence==0)assert(delta.z==0);
        if(action==ClaySurfaceAction::Scrape)assert(delta.z<=1e-12);
        l1+=std::abs(delta.z);v[i].position+=delta;
      }
      if(l1>1e-9)++moving;
      if(action==ClaySurfaceAction::Press || action==ClaySurfaceAction::Melt)
        assert(std::abs(out.signed_swept_volume)<1e-8);
      else assert(out.signed_swept_volume<=1e-10);
    }
    std::cout<<"V27B overlap action="<<int(action)<<" moving="<<moving<<std::endl;
    assert(moving>=6);
  }
}
int main(){
  stroke_context_v27_tests();
  contact_v27b_tests();
  assert(progressive_press_depth_v26(.07,0)==.07);
  assert(progressive_press_depth_v26(.07,.5)>progressive_press_depth_v26(.07,.1));
  assert(progressive_press_depth_v26(.24,100)<=.25);
  for(auto action:{ClaySurfaceAction::Buildup,ClaySurfaceAction::Push,ClaySurfaceAction::Gather,
      ClaySurfaceAction::Spread,ClaySurfaceAction::FusionDeposit,ClaySurfaceAction::Compress,
      ClaySurfaceAction::Compact,ClaySurfaceAction::Press,ClaySurfaceAction::PressureFlow,
      ClaySurfaceAction::HybridFlow,ClaySurfaceAction::Melt,ClaySurfaceAction::Smear,
      ClaySurfaceAction::Scrape})overlapping_v26(action);
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
