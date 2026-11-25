#include "global_collision_v29.hh"
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
static void stability_v28_tests(){
  assert(history_scale_v28({0,0,.25},{},{0,0,.1},.25)<1e-12);
  assert(history_scale_v28({0,0,.25},{},{0,0,-.1},.25)==1);
  assert(std::abs(history_scale_v28({0,0,.20},{},{0,0,.10},.25)-.5)<1e-12);
  for(auto action:{ClaySurfaceAction::Press,ClaySurfaceAction::Compress,ClaySurfaceAction::Compact,
                   ClaySurfaceAction::Scrape,ClaySurfaceAction::Push,ClaySurfaceAction::Gather,
                   ClaySurfaceAction::Smear,ClaySurfaceAction::Melt}){
    constexpr int n=33;
    std::vector<ClaySurfaceVertex> v;
    std::vector<ClaySurfaceTriangle> tri;
    const double radius=.5;
    for(int y=0;y<n;++y)for(int x=0;x<n;++x){
      const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
      const Vec3 p{px,py,.015*std::cos(8*px)*std::cos(7*py)};
      v.push_back({p,0,p,radius});
    }
    for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
      const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
      tri.push_back({a,b,d});tri.push_back({a,d,c});
    }
    ClaySurfaceDab dab;dab.action=action;dab.radius=radius;dab.intensity=2;dab.dose=.30;
    dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
    dab.tool_v25=true;dab.tool_v26=true;dab.fast_v26b=true;dab.contact_v27b=true;dab.stable_v28=true;
    dab.has_tool_plane=true;dab.tool_plane_height=-.07*radius;
    double total_motion=0;
    for(int k=0;k<48;++k){
      const double x=.16*std::sin(.23*k),y=.12*std::cos(.23*k);
      dab.center={x,y,0};dab.direction={std::cos(.23*k),-std::sin(.23*k),0};
      for(auto &p:v){
        const double dx=p.position.x-x,dy=p.position.y-y;
        const double r2=(dx*dx+dy*dy)/(radius*radius);
        p.influence=r2<1&&p.position.y>-.23?std::pow(1-r2,2):0;
      }
      const auto out=solve_material_transport_v21(v,tri,dab,4,false).surface;
      if(action==ClaySurfaceAction::Push || action==ClaySurfaceAction::Gather ||
         action==ClaySurfaceAction::Smear)assert(out.trace_attempts==0);
      for(std::size_t i=0;i<v.size();++i){
        const auto delta=out.displacement[i];
        assert(std::isfinite(delta.z));
        if(v[i].influence==0)assert(delta.z==0);
        if(action==ClaySurfaceAction::Scrape)assert(delta.z<=1e-12);
        v[i].position+=delta;
        assert(std::abs(v[i].position.z-v[i].contact_reference.z)<=radius*history_radius_fraction_v28(action)+1e-10);
        total_motion+=std::abs(delta.z);
      }
      if(action==ClaySurfaceAction::Scrape || action==ClaySurfaceAction::Compact)
        assert(out.signed_swept_volume<=1e-9);
      else assert(std::abs(out.signed_swept_volume)<1e-8);
    }
    assert(total_motion>1e-6);
    std::cout<<"V28 stability action="<<int(action)<<" motion="<<total_motion<<std::endl;
  }
}
static void refinement_v28b_tests(){
  constexpr int n=33;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
    const double r2=(px*px+py*py)/.64;
    const Vec3 p{px,py,.02+.03*px-.04*py-.10*(px*px+py*py)+.02*px*py};
    v.push_back({p,r2<1?std::pow(1-r2,2):0,p,.8});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.action=ClaySurfaceAction::Melt;dab.radius=.8;dab.dose=.15;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.contact_v27b=true;dab.stable_v28=true;dab.refine_v28b=true;
  const auto equilibrium=solve_material_transport_v21(v,tri,dab,4,false).surface;
  for(const auto &p:equilibrium.displacement)assert(std::abs(p.z)<1e-10);
  for(auto &p:v)p.position.z+=.04*std::exp(-100*(p.position.x*p.position.x+p.position.y*p.position.y));
  const auto relief=solve_material_transport_v21(v,tri,dab,4,false).surface;
  assert(relief.displacement[n*n/2].z<0);
  assert(std::abs(relief.signed_swept_volume)<1e-9);
  StrokeContextV27 first;
  auto step=stage_stroke_v27(first,{0,0,0},{0,0,1},1,1);
  assert(track_tool_plane_v27(step,first,{}));
  first=step.next;
  step=stage_stroke_v27(first,{.1,0,0},{0,0,1},1,1);
  std::vector<PhysicalBrushIntentSample> narrow{{0,{.20,.30,-.03},1,1,true,{0,0,1}}};
  auto legacy=step;
  assert(!track_tool_plane_v27(legacy,first,narrow));
  assert(track_tool_plane_v27(step,first,narrow,true));
  assert(step.next.tool_reference.z<0 && step.next.tool_reference.z>=-.04);
}
static void swept_v29_tests(){
  ClaySurfaceDab dab;dab.center={.4,0,0};dab.radius=1;dab.sweep_start={0,0,0};dab.swept_v29=true;
  assert(std::abs(chart_distance_v29({-1.3,0,0},dab,{0,0,1})-.9)<1e-12);
  dab.swept_v29=false;
  assert(chart_distance_v29({-1.3,0,0},dab,{0,0,1})>1);
  dab.swept_v29=true;
  assert(std::abs(chart_distance_v29({-.2,.5,0},dab,{0,0,1})-.5)<1e-12);
}
static void compaction_v29_tests(){
  double state=0;
  for(int stroke=0;stroke<5;++stroke){
    const double next=update_compaction_v29(state,-.05,1,1);
    assert(next>state && next<1);state=next;
  }
  assert(update_compaction_v29(state,.05,1,1)==state);
  assert(update_compaction_v29(state,.05,1,-1)<state);
  assert(update_compaction_v29(state,0,1,1)==state);
}
static void collision_v29_tests(){
  const std::vector<Vec3> positions{{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,0,1},{0,1,1}};
  std::vector<Vec3> motion(6);
  const std::vector<std::array<std::size_t,3>> tri{{0,1,2},{3,4,5}};
  const auto tr=[&](std::size_t i){return tri.at(i);};
  const auto p=[&](std::size_t i){return positions.at(i);};
  const auto m=[&](std::size_t i){return motion.at(i);};
  assert(guard_global_collision_v29(tri.size(),tr,p,m).scale==1);
  for(int i=0;i<3;++i)motion[i]={0,0,2};
  const auto crossing=guard_global_collision_v29(tri.size(),tr,p,m);
  assert(crossing.scale>0 && crossing.scale<.5 && crossing.limited_pairs==1);
  for(int i=0;i<3;++i)motion[i]={0,0,-2};
  assert(guard_global_collision_v29(tri.size(),tr,p,m).scale==1);
}
static void live_surface_v30_tests(){
  // A patch whose prior sculpting already exceeded the old stroke-start
  // budget must still permit a new incremental unilateral scrape.
  constexpr int n=25;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
    const double r2=(px*px+py*py)/.64;
    v.push_back({{px,py,-.6},r2<1?std::pow(1-r2,2):0,{px,py,0},.8});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.action=ClaySurfaceAction::Scrape;dab.radius=.8;dab.dose=.15;
  dab.center={0,0,-.6};dab.has_tool_plane=true;dab.tool_plane_height=-.05;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;dab.refine_v28b=true;
  const auto old=solve_material_transport_v21(v,tri,dab,4,false).surface;
  assert(std::abs(old.displacement[n*n/2].z)<1e-12);
  dab.live_surface_v30=true;
  const auto first=solve_material_transport_v21(v,tri,dab,4,false).surface;
  assert(first.displacement[n*n/2].z< -1e-6 && first.history_limited==0);
  // Compare stale-history input against an independent fresh geometry.
  auto fresh=v;for(auto &p:fresh){p.contact_radius=0;p.contact_reference=p.position;}
  const auto same=solve_material_transport_v21(fresh,tri,dab,4,false).surface;
  for(std::size_t i=0;i<v.size();++i){
    assert(std::abs(first.displacement[i].z-same.displacement[i].z)<1e-12);
    if(v[i].influence==0)assert(first.displacement[i].z==0);
    v[i].position+=first.displacement[i];
  }
  dab.center=v[n*n/2].position;
  const auto second=solve_material_transport_v21(v,tri,dab,4,false).surface;
  assert(second.displacement[n*n/2].z< -1e-6);
}
static void transport_v30b_tests(){
  StrokeContextV27 state;
  auto step=stage_stroke_v27(state,{0,0,0},{0,0,1},1,1);
  stage_transport_v30b(step,state);state=step.next;
  step=stage_stroke_v27(state,{.08,0,0},{0,0,1},1,1);
  stage_transport_v30b(step,state);state=step.next;
  const double load=state.transport_load_v30b;
  assert(load>0 && state.transport_direction_v30b.x>.99);
  step=stage_stroke_v27(state,{.16,.02,0},{0,0,1},1,1);
  stage_transport_v30b(step,state);
  assert(step.next.transport_load_v30b>load && step.next.transport_direction_v30b.x>.95);
  constexpr int n=41;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1,r2=(px*px+py*py)/.64;
    v.push_back({{px,py,.008*std::sin(60*px)*std::cos(45*py)},r2<1 && py>-.2?std::pow(1-r2,2):0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.radius=.8;dab.dose=.15;dab.direction={1,0,0};
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;dab.refine_v28b=true;
  dab.live_surface_v30=true;dab.transport_v30b=true;dab.fast_v26b=true;
  for(auto action:{ClaySurfaceAction::Push,ClaySurfaceAction::Gather,ClaySurfaceAction::Smear}){
    dab.action=action;auto work=v;double motion=0;
    for(int k=0;k<12;++k){
      const auto out=solve_material_transport_v21(work,tri,dab,4,false).surface;
      assert(std::abs(out.signed_swept_volume)<1e-9);
      if(action==ClaySurfaceAction::Smear)assert(out.fine_transport_v30b>0);
      for(std::size_t i=0;i<work.size();++i){
        assert(std::isfinite(out.displacement[i].z));
        if(work[i].influence==0)assert(out.displacement[i].z==0);
        work[i].position+=out.displacement[i];motion+=std::abs(out.displacement[i].z);
      }
    }
    assert(motion>1e-6);
  }
}
static void contact_v30b_tests(){
  constexpr int n=41;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1,r2=(px*px+py*py)/.64;
    v.push_back({{px,py,-.12*std::exp(-80*(px*px+py*py))},r2<1?std::pow(1-r2,2):0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.radius=.8;dab.dose=.15;dab.direction={1,0,0};
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;dab.refine_v28b=true;
  dab.live_surface_v30=true;dab.fast_v26b=true;dab.contact_v27b=true;
  dab.has_tool_plane=true;dab.tool_plane_height=-.05;
  dab.action=ClaySurfaceAction::Scrape;
  const auto old=solve_clay_surface(v,tri,dab);
  assert(old.displacement[n*n/2].z==0);
  dab.contact_v30b=true;
  const auto cut=solve_clay_surface(v,tri,dab);
  assert(cut.displacement[n*n/2].z<0);
  for(const auto &p:cut.displacement)assert(p.z<=1e-12);
  v[std::size_t((n/2+4)*n+n/2+4)].position.z=.8;
  const auto recovered=solve_clay_surface(v,tri,dab);
  assert(recovered.chart_recovered_v30b>0);
  for(auto action:{ClaySurfaceAction::Press,ClaySurfaceAction::Compress,ClaySurfaceAction::Compact}){
    dab.action=action;const auto out=solve_clay_surface(v,tri,dab);
    double motion=0;
    for(std::size_t i=0;i<v.size();++i){
      assert(std::isfinite(out.displacement[i].z));
      if(v[i].influence==0)assert(out.displacement[i].z==0);
      motion+=std::abs(out.displacement[i].z);
    }
    assert(motion>1e-8);
    if(action==ClaySurfaceAction::Compact)assert(out.signed_swept_volume<=1e-9);
    else assert(std::abs(out.signed_swept_volume)<1e-9);
  }
}
static void overlap_v30c_tests(){
  constexpr int n=41;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1,r2=(px*px+py*py)/.64;
    // One zero-factor column on a connected sheet; both sides must work,
    // but the excluded column must not receive or transport matter.
    v.push_back({{px,py,0},r2<1 && x!=n/2?std::pow(1-r2,2):0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.radius=.8;dab.dose=.15;dab.direction={1,0,0};
  dab.action=ClaySurfaceAction::Scrape;dab.has_tool_plane=true;dab.tool_plane_height=-.05;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;dab.refine_v28b=true;
  dab.live_surface_v30=true;dab.fast_v26b=true;dab.contact_v30b=true;
  const auto old=solve_clay_surface(v,tri,dab);
  assert(old.component_rejected_v30b>0);
  dab.overlap_v30c=true;
  const auto cut=solve_clay_surface(v,tri,dab);
  assert(cut.patches_v30c==2 && cut.active>old.active);
  double left=0,right=0;
  for(std::size_t i=0;i<v.size();++i){
    if(v[i].influence==0)assert(cut.displacement[i].z==0);
    if(v[i].position.x<0)left-=cut.displacement[i].z;
    if(v[i].position.x>0)right-=cut.displacement[i].z;
  }
  assert(left>0 && right>0);
  // Separate sheet is not activated just because it is close in projection.
  const auto start=v.size();
  v.push_back({{.1,.1,.03},1});v.push_back({{.2,.1,.03},1});v.push_back({{.1,.2,.03},1});
  tri.push_back({start,start+1,start+2});
  const auto sheets=solve_clay_surface(v,tri,dab);
  for(std::size_t i=start;i<v.size();++i)assert(sheets.displacement[i].z==0);
}
static void push_v30d_tests(){
  constexpr int n=49;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
    const double r2=(px*px+py*py)/.64;
    const double h=.04*std::exp(-24*((px+.12)*(px+.12)+py*py));
    v.push_back({{px,py,h},r2<1 && py>-.45?std::pow(1-r2,2):0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  ClaySurfaceDab dab;dab.radius=.8;dab.dose=.08;dab.direction={1,0,0};
  dab.action=ClaySurfaceAction::Push;dab.intensity=1;dab.transport_load_v30b=.8;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;dab.refine_v28b=true;
  dab.live_surface_v30=true;dab.fast_v26b=true;dab.transport_v30b=true;
  const auto old=solve_clay_surface(v,tri,dab);
  dab.push_v30d=true;
  const auto first=solve_clay_surface(v,tri,dab);
  const auto moment=[&](const ClaySurfaceResult &out){
    double m=0;
    for(std::size_t i=0;i<v.size();++i)m+=out.projected_area[i]*out.displacement[i].z*v[i].position.x;
    return m;
  };
  assert(moment(first)>moment(old) && moment(first)>0);
  assert(std::abs(first.signed_swept_volume)<1e-9);
  auto work=v;double accumulated=0;
  for(int k=0;k<12;++k){
    const auto out=solve_material_transport_v21(work,tri,dab,4,false).surface;
    assert(std::abs(out.signed_swept_volume)<1e-9);
    double forward=0;
    for(std::size_t i=0;i<work.size();++i){
      assert(std::isfinite(out.displacement[i].z));
      if(work[i].influence==0)assert(out.displacement[i].z==0);
      forward+=out.projected_area[i]*out.displacement[i].z*v[i].position.x;
      work[i].position+=out.displacement[i];
    }
    if(k==11)assert(forward>0);
    accumulated+=forward;
  }
  assert(accumulated>moment(first));
  // Actual zero dose and masked input must remain inert.
  dab.dose=0;
  const auto none=solve_clay_surface(work,tri,dab);
  for(const auto &p:none.displacement)assert(p.z==0);
}
static void adaptive_v31_tests(){
  // Inclined front-facing strip: points beyond the old depth slab must
  // still be reachable along current mesh edges; a separate sheet must not.
  constexpr int n=41;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
    const double r2=(px*px+py*py)/.64;
    v.push_back({{px,py,1.2*px},r2<1?std::pow(1-r2,2):0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tri.push_back({a,b,d});tri.push_back({a,d,c});
  }
  const auto other=v.size();
  v.push_back({{.05,.05,.12},1});v.push_back({{.15,.05,.12},1});v.push_back({{.05,.15,.12},1});
  tri.push_back({other,other+1,other+2});
  ClaySurfaceDab dab;dab.radius=.8;dab.dose=.10;dab.direction={1,0,0};
  dab.action=ClaySurfaceAction::Scrape;dab.has_tool_plane=true;dab.tool_plane_height=-.05;
  dab.density_independent=true;dab.smooth_proxy=true;dab.shape_transport=true;
  dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;dab.refine_v28b=true;
  dab.live_surface_v30=true;dab.fast_v26b=true;dab.contact_v30b=true;
  const auto old=solve_clay_surface(v,tri,dab);
  dab.adaptive_v31=true;
  const auto out=solve_clay_surface(v,tri,dab);
  assert(out.adaptive_deep_v31>0 && out.active>old.active);
  bool deep_moved=false;
  for(std::size_t i=0;i<v.size();++i){
    const auto q=out.displacement[i];
    assert(std::isfinite(q.z));
    assert(std::abs(q.x)<1e-14 && std::abs(q.y)<1e-14);
    if(i>=other || v[i].influence==0)assert(q.x==0 && q.y==0 && q.z==0);
    if(i<other && std::abs(v[i].position.z)>.65*dab.radius && q.z<0)deep_moved=true;
    assert(q.z<=1e-12);
  }
  assert(deep_moved);
  for(auto action:{ClaySurfaceAction::Press,ClaySurfaceAction::Compress,ClaySurfaceAction::Compact}){
    dab.action=action;
    const auto step=solve_clay_surface(v,tri,dab);
    assert(step.active>0 && step.transported_volume>0);
    if(action!=ClaySurfaceAction::Compact)assert(std::abs(step.signed_swept_volume)<1e-9);
    else assert(step.signed_swept_volume<=1e-9);
  }
}
static void coherent_v31b_tests(){
  // Triangle with a very short altitude: differential displacement must
  // be constrained even when the original area guard alone would allow it.
  std::vector<Vec3> p{{0,0,0},{1,0,0},{.5,.02,0}};
  std::vector<Vec3> q{{0,0,0},{0,0,0},{0,0,.10}};
  std::vector<ClaySurfaceTriangle> tri{{0,1,2}};
  const double scale=coherent_motion_scale_v31b(p,tri,q);
  assert(scale>0 && scale<=.036000001);
  for(auto &v:q)v=v*scale;
  assert(coherent_motion_scale_v31b(p,tri,q)>=.999999);
  // Rigid translation is not deformation and must not be constrained.
  for(auto &v:q)v={0,0,.1};
  assert(coherent_motion_scale_v31b(p,tri,q)==1);
}
static void gradient_v31c_tests(){
  const std::vector<Vec3> p{{0,0,0},{1,0,0},{.5,.02,0}};
  const std::vector<ClaySurfaceTriangle> tri{{0,1,2}};
  // Displacement gradient along the long edge is mild despite small altitude.
  std::vector<Vec3> q{{0,0,0},{0,0,.10},{0,0,.05}};
  assert(coherent_motion_scale_v31b(p,tri,q)<.04);
  assert(gradient_motion_scale_v31c(p,tri,q)==1);
  // Perpendicular gradient remains tightly constrained.
  q={{0,0,0},{0,0,0},{0,0,.10}};
  const double scale=gradient_motion_scale_v31c(p,tri,q);
  assert(std::abs(scale-.036)<1e-10);
  for(auto &v:q)v=v*scale;
  assert(gradient_motion_scale_v31c(p,tri,q)>=.999999);
  for(auto &v:q)v={1,2,3};
  assert(gradient_motion_scale_v31c(p,tri,q)==1);
  // Rank-one affine support: exactly preserve a slope along the line.
  std::vector<Vec3> xy;std::vector<double> h,w,a;
  for(int i=0;i<11;++i){
    const double x=(i-5)*.1;
    xy.push_back({x,x,0});h.push_back(.3+.7*x);w.push_back(1);a.push_back(1);
  }
  std::array<double,6> coeff{};
  assert(!fit_quadratic_v28b(xy,h,w,a,{1,0,0},{0,1,0},1,coeff));
  assert(fit_affine_v31c(xy,h,w,a,{1,0,0},{0,1,0},1,coeff));
  for(std::size_t i=0;i<h.size();++i)
    assert(std::abs(h[i]-quadratic_height_v28b(coeff,xy[i].x,xy[i].y))<1e-10);
}
static void average_v31d_tests(){
  // Affine support with an isolated high outlier: reference is not
  // the raw noisy geometry. Planar background must remain recognizable.
  std::vector<Vec3> xy;std::vector<double> h,w,a,ref;
  for(int y=0;y<9;++y)for(int x=0;x<9;++x){
    const double px=(x-4)*.1,py=(y-4)*.1;
    xy.push_back({px,py,0});h.push_back(.1+.5*px-.2*py);w.push_back(1);a.push_back(1);
  }
  h[40]+=.6;
  assert(average_reference_v31d(xy,h,w,a,{0,0,1},1,ref));
  assert(std::abs(ref[40]-.1)<.02 && h[40]>.69);
  // Transport on an inclined strip: the reference should admit some
  // points beyond the old slab while the real mesh stays untouched by
  // reference construction. Detached sheet must remain stationary.
  constexpr int n=31;
  std::vector<ClaySurfaceVertex> v;std::vector<ClaySurfaceTriangle> tri;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const double px=2.*x/(n-1)-1,py=2.*y/(n-1)-1;
    const double r2=(px*px+py*py)/.64;
    v.push_back({{px,py,.9*px},r2<1?std::pow(1-r2,2):0});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x){
    const auto i=std::size_t(y*n+x),j=i+1,k=i+n,l=k+1;
    tri.push_back({i,j,l});tri.push_back({i,l,k});
  }
  const auto other=v.size();
  v.push_back({{.05,.05,.16},1});v.push_back({{.15,.05,.16},1});v.push_back({{.05,.15,.16},1});
  tri.push_back({other,other+1,other+2});
  ClaySurfaceDab dab;dab.radius=.8;dab.dose=.10;dab.direction={1,0,0};
  dab.action=ClaySurfaceAction::Push;dab.density_independent=true;dab.smooth_proxy=true;
  dab.shape_transport=true;dab.tool_v25=true;dab.tool_v26=true;dab.stable_v28=true;
  dab.refine_v28b=true;dab.live_surface_v30=true;dab.fast_v26b=true;dab.overlap_v30c=true;
  dab.transport_v30b=true;dab.push_v30d=true;
  const auto old=solve_clay_surface(v,tri,dab);
  dab.average_surface_v31d=.65;
  const auto out=solve_clay_surface(v,tri,dab);
  assert(out.average_fits_v31d>0 && out.average_recovered_v31d>0 && out.active>old.active);
  for(std::size_t i=other;i<v.size();++i)
    assert(out.displacement[i].x==0 && out.displacement[i].y==0 && out.displacement[i].z==0);
}
static void continuous_v31d2_tests(){
  // Sharp neighboring slopes with individually positive projected
  // support remain connected; a masked bridge and a separate sheet do not.
  const std::vector<Vec3> p{{0,0,0},{.10,0,.08},{.20,0,0},{.30,0,0},{.40,0,0},{.05,.05,.01}};
  const std::vector<Vec3> dual{{.9,0,.1},{-.9,0,.1},{.9,0,.1},{0,0,1},{0,0,1},{0,0,1}};
  const ClaySurfaceEdges edges{{0,1},{1,2},{2,3},{3,4}};
  std::vector<double> w{1,1,1,0,1,1};
  std::size_t rejected=0;
  connected_contact_v31d2(p,dual,edges,{0,0,1},0,0,1.5,1,.65,w,rejected);
  assert(w[0]>0 && w[1]>0 && w[2]>0);
  assert(w[3]==0 && w[4]==0 && w[5]==0 && rejected==2);
  // On a smooth parabola the bounded curved reference retains more
  // shape than the affine-only D1 reference.
  std::vector<Vec3> xy;std::vector<double> h,weights,area,flat,curved;
  for(int y=0;y<9;++y)for(int x=0;x<9;++x){
    const double px=(x-4)*.1,py=(y-4)*.1;
    xy.push_back({px,py,0});h.push_back(.6*(px*px+py*py));
    weights.push_back(1);area.push_back(1);
  }
  assert(average_reference_v31d(xy,h,weights,area,{0,0,1},1,flat));
  assert(average_reference_v31d(xy,h,weights,area,{0,0,1},1,curved,true));
  double e0=0,e1=0;
  for(std::size_t i=0;i<h.size();++i){
    e0+=std::abs(h[i]-flat[i]);e1+=std::abs(h[i]-curved[i]);
  }
  assert(e1<e0);
}
static void motion_proxy_v31d3_tests(){
  const std::vector<ClaySurfaceVertex> v{
      {{.01,.01,0},1},{{.03,.01,0},1},{{.05,.01,0},1},
      {{.07,.01,0},0},{{.09,.01,0},1},{{.04,.01,.001},1}};
  const ClaySurfaceEdges edges{{0,1},{1,2},{2,3},{3,4}};
  const std::vector<double> w{1,.5,1,0,1,1},area{1,2,3,1,1,1};
  const std::vector<double> delta{.03,-.02,.01,0,.04,-.05};
  std::vector<double> f;
  assert(motion_proxy_level_v31d3(v,edges,w,area,delta,{},.12,f)>0);
  double b=0,a=0;
  for(std::size_t i=0;i<v.size();++i){b+=area[i]*delta[i];a+=area[i]*f[i];}
  assert(std::abs(b-a)<1e-12);
  assert(f[3]==0 && std::abs(f[4]-.04)<1e-12 && std::abs(f[5]+.05)<1e-12);
  assert(std::abs(f[0]-f[1])<std::abs(delta[0]-delta[1]));
  std::vector<double> constant(w.size());
  for(std::size_t i=0;i<w.size();++i)constant[i]=.02*w[i];
  assert(motion_proxy_level_v31d3(v,edges,w,area,constant,{},.25,f)>0);
  for(std::size_t i=0;i<w.size();++i)assert(std::abs(f[i]-constant[i])<1e-12);
  ClaySurfaceDab dab;dab.radius=1;
  ClaySurfaceResult out;auto smoothed=delta;
  reconstruct_motion_v31d3(v,edges,w,area,dab,.65,smoothed,out);
  assert(out.proxy_passes_v31d3==1 && out.proxy_rejected_v31d3==0);
  assert(out.proxy_budget_error_v31d3<1e-12);
  assert(out.proxy_strain_after_v31d3<=out.proxy_strain_before_v31d3*1.02+1e-12);
  assert(smoothed[3]==0 && std::abs(smoothed[4]-.04)<1e-12);
}
int main(){
  stroke_context_v27_tests();
  motion_proxy_v31d3_tests();
  continuous_v31d2_tests();
  average_v31d_tests();
  gradient_v31c_tests();
  coherent_v31b_tests();
  adaptive_v31_tests();
  push_v30d_tests();
  overlap_v30c_tests();
  contact_v30b_tests();
  transport_v30b_tests();
  live_surface_v30_tests();
  collision_v29_tests();
  compaction_v29_tests();
  swept_v29_tests();
  refinement_v28b_tests();
  stability_v28_tests();
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
