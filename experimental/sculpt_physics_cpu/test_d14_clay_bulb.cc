/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d14_clay_bulb.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics;
namespace b=dedicated_d14::bulb;
namespace d=dedicated_d12;
static void check(bool yes,const char* s){if(!yes){std::fprintf(stderr,"D14 bulb: %s\n",s);std::abort();}}
static PhysicalBrushIntentSample sample(Vec3 p,double w=1.,Vec3 n={0,0,1}){
 return {0,p,w,1.,true,n};
}
int main(){
 check(b::asset_name("Clay Bulb.001")&&b::asset_name("ClayBulb")&&!b::asset_name("Clay Strips"),"asset routing");
 b::Kernel k{{},{},{0,0,1},1.,.06,3.,1.};
 auto s=sample({});
 const d::ClayReference ref{s.position,s.surface_normal,1.};
 const double a=b::deposit(k,s,ref).z;
 check(a>.035&&a<.06,"initial strength 3");
 double prev=a;
 for(int i=1;i<=100;++i){
  auto q=sample({i/100.,0,0});
  const double h=b::deposit(k,q,{q.position,q.surface_normal,1.}).z;
  check(h<=prev+1e-12&&h>=0.,"convex monotonic tip");prev=h;
 }
 check(prev==0.,"edge zero");
 auto q=sample({.99,0,0});
 check(b::deposit(k,q,{q.position,q.surface_normal,1.}).z<1e-5,"C2 edge");
 check(d::norm2(b::deposit(k,sample({0,0,-.8}),ref))==0.,"deep rejection");
 check(d::norm2(b::deposit(k,sample({},0.),ref))==0.,"mask");
 check(d::norm2(b::deposit(k,sample({},1.,{0,0,-1}),ref))==0.,"backface");
 auto masked=s;masked.writable=false;
 check(d::norm2(b::deposit(k,masked,ref))==0.,"write permission");
 for(int i=0;i<100;++i){k.end=s.position;k.start=k.end;s.position+=b::deposit(k,s,ref);}
 check(s.position.z>.27&&s.position.z<=.30+1e-12,"bounded accumulation");
 const d::ClayReference next{s.position,s.surface_normal,1.};
 check(b::deposit(k,s,next).z>0.,"new stroke buildup");
 // The flat, fixed-position integral is additive for partitioned path.
 b::Kernel full{{-.7,0,0},{.7,0,0},{0,0,1},1.,1.4,1.,1.};
 double integrated=0.;
 for(int i=0;i<20;++i){
  auto sub=full;
  sub.start={-.7+i*.07,0,0};sub.end={-.7+(i+1)*.07,0,0};sub.dose=.07;
  integrated+=sub.dose*b::profile_integral(sub,{.2,.3,0});
 }
 check(std::abs(integrated-full.dose*b::profile_integral(full,{.2,.3,0}))<1e-12,"swept dose quadrature");
 // Existing step influences new material exchange, without removing old
 // material, and without writing zero-envelope or unconnected samples.
 std::vector<PhysicalBrushIntentSample> patch{sample({0,0,.2}),sample({.1,0,0}),sample({.2,0,0},0.)};
 std::vector<Vec3> motion{{0,0,.04},{0,0,.02},{}};
 double moved=0.;
 check(b::fuse(k,patch,{{0,1},{1,2}},{1.,2.,1.},{.1,.1,0.},motion,nullptr,nullptr,&moved),"fuse");
 check(moved>0.&&motion[0].z>0.&&motion[0].z<.04&&motion[1].z>.02&&
       std::abs(motion[0].z+2*motion[1].z-.08)<1e-12&&motion[2].z==0.,"conservative new deposit");
 std::puts("D14_CLAY_BULB_REFERENCE_OK");
}
