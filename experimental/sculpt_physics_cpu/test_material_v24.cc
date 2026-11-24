/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

using namespace sculpt_physics;
struct Patch {
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  int n=0;
};
static Patch make_patch(int n,double angle=0,bool flat=false)
{
  Patch p;p.n=n;
  const Vec3 dir{std::cos(angle),std::sin(angle),0};
  for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
    const double px=2.0*x/(n-1)-1,py=2.0*y/(n-1)-1;
    const double rr=(px*px+py*py)/(.8*.8);
    const double w=rr<1?std::pow(1-rr,2):0;
    const double bx=px+.16*dir.x,by=py+.16*dir.y;
    p.v.push_back({{px,py,flat?0:.10*std::exp(-(bx*bx+by*by)/(.085*.085))},w});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x) {
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    if((x+y)%2){p.t.push_back({a,b,c});p.t.push_back({b,d,c});}
    else{p.t.push_back({a,b,d});p.t.push_back({a,d,c});}
  }
  return p;
}
static ClaySurfaceDab settings(ClaySurfaceAction action,double angle=0)
{
  ClaySurfaceDab d;d.action=action;d.radius=.8;d.dose=.12;
  d.intensity=1;d.mobility=.85;d.friction=.85;d.plasticity=.95;
  d.viscosity=.2;d.volume_preservation=.75;
  d.direction={std::cos(angle),std::sin(angle),0};
  d.density_independent=true;d.smooth_proxy=true;d.shape_transport=true;
  return d;
}
static double l1(const ClaySurfaceResult &r)
{
  double out=0;
  for(std::size_t i=0;i<r.displacement.size();++i)
    out+=std::abs(r.displacement[i].z)*r.projected_area[i];
  return out;
}
static ClaySurfaceResult apply(Patch &p,ClaySurfaceDab d)
{
  const auto r=solve_material_transport_v21(p.v,p.t,d,4).surface;
  double volume=0;
  for(std::size_t i=0;i<p.v.size();++i) {
    const auto &m=r.displacement[i];
    assert(m.x==0 && m.y==0 && std::isfinite(m.z));
    if(p.v[i].influence==0)assert(m.z==0);
    volume+=m.z*r.projected_area[i];p.v[i].position+=m;
  }
  assert(std::abs(volume-r.signed_swept_volume)<1e-9);
  if(d.action!=ClaySurfaceAction::FusionDeposit && d.action!=ClaySurfaceAction::Compact)
    assert(std::abs(volume)<1e-9);
  return r;
}
struct TransportMetric {double shift=0,old_peak_fraction=0,mass=0;};
static TransportMetric transport(int n,double angle)
{
  auto p=make_patch(n,angle);
  const auto initial=p.v;
  auto d=settings(ClaySurfaceAction::Push,angle);
  ClaySurfaceResult r;
  for(int i=0;i<6;++i)r=apply(p,d);
  double mass=0,first=0,last=0,max_old=0;std::size_t id=0;
  for(std::size_t i=0;i<p.v.size();++i) {
    const double a=r.projected_area[i];
    const auto &q=p.v[i].position;
    const double x=q.x*std::cos(angle)+q.y*std::sin(angle);
    mass+=a*initial[i].position.z;
    first+=x*a*initial[i].position.z;last+=x*a*q.z;
    if(initial[i].position.z>max_old){max_old=initial[i].position.z;id=i;}
  }
  assert(mass>0);
  TransportMetric m{(last-first)/mass,p.v[id].position.z/max_old,mass};
  std::cout<<"flow n="<<n<<" angle="<<angle<<" shift="<<m.shift
           <<" old_peak_fraction="<<m.old_peak_fraction
           <<" trace_reject="<<r.trace_rejected<<"/"<<r.trace_attempts<<std::endl;
  assert(m.shift>.04 && m.old_peak_fraction<.90);
  return m;
}
int main()
{
  const auto coarse=transport(49,0),fine=transport(97,0);
  const auto diagonal=transport(97,.7853981633974483);
  assert(fine.shift/coarse.shift>.65 && fine.shift/coarse.shift<1.55);
  assert(diagonal.shift/fine.shift>.8 && diagonal.shift/fine.shift<1.2);

  const auto patch=make_patch(65);
  double previous=0;
  for(double strength:{0.0,.5,1.0,2.0}) {
    auto p=patch;auto d=settings(ClaySurfaceAction::Compress);d.intensity=strength;
    const auto r=apply(p,d);const double value=l1(r);
    std::cout<<"strength="<<strength<<" compress_l1="<<value<<std::endl;
    if(strength==0)assert(value==0);
    else assert(value>previous*1.05);
    previous=value;
  }

  // Every action must have a recognizable geometric signature.
  auto flat=make_patch(65,0,true);
  auto gathered=flat;
  const auto gather=apply(gathered,settings(ClaySurfaceAction::Gather));
  const auto center=std::size_t(65*32+32);
  assert(gather.displacement[center].z>0 && l1(gather)>1e-7);
  auto compacted=flat;
  const auto compact=apply(compacted,settings(ClaySurfaceAction::Compact));
  assert(compact.signed_swept_volume<0 && compact.displacement[center].z<0);
  auto pressed=patch,compressed=patch;
  const auto press=apply(pressed,settings(ClaySurfaceAction::Press));
  const auto compress=apply(compressed,settings(ClaySurfaceAction::Compress));
  assert(press.displacement[center].z<0 && compress.displacement[center].z<0);
  assert(l1(press)>1e-7 && l1(compress)>1e-7);
  auto flow_patch=patch,smear_patch=patch,melt_patch=patch;
  const auto flow=apply(flow_patch,settings(ClaySurfaceAction::Push));
  const auto smear=apply(smear_patch,settings(ClaySurfaceAction::Smear));
  const auto melt=apply(melt_patch,settings(ClaySurfaceAction::Melt));
  double difference=0,melt_difference=0,peak_before=0,peak_after=0;
  for(std::size_t i=0;i<patch.v.size();++i) {
    difference+=std::abs(flow.displacement[i].z-smear.displacement[i].z)*flow.projected_area[i];
    melt_difference+=std::abs(melt.displacement[i].z-smear.displacement[i].z)*flow.projected_area[i];
    peak_before=std::max(peak_before,patch.v[i].position.z);
    peak_after=std::max(peak_after,melt_patch.v[i].position.z);
  }
  assert(difference>1e-7 && melt_difference>1e-7 && peak_after<peak_before);
  // Overlap actual accumulated Deposit with Flow and then Smear.
  auto overlap=flat;
  auto deposit=settings(ClaySurfaceAction::FusionDeposit);deposit.shape_transport=false;
  deposit.dose=.15;
  const auto added=apply(overlap,deposit);
  assert(added.signed_swept_volume>0);
  for(int i=0;i<4;++i) {
    auto move=settings(ClaySurfaceAction::Push);move.center={.035*i,0,0};
    assert(l1(apply(overlap,move))>1e-8);
  }
  auto blend=settings(ClaySurfaceAction::Smear);blend.center={.08,0,0};
  assert(l1(apply(overlap,blend))>1e-8);
}
