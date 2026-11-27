/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d14_continuous_fusion.hh"
#include "d13_clay_fusion.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics;
namespace c=dedicated_d14::continuous_clay;
namespace f=dedicated_d14::continuous_fusion;
namespace m=dedicated_d14::median_fusion;
namespace d=dedicated_d12;
static void check(bool yes,const char* s){if(!yes){std::fprintf(stderr,"continuous clay: %s\n",s);std::abort();}}
static PhysicalBrushIntentSample sample(Vec3 p,double w=1.,Vec3 n={0,0,1}){
 return {0,p,w,1.,true,n};
}
int main(){
 c::Kernel q{{},{},{0,0,1},{1,0,0},1.,.04,3.,1.,.27};
 auto s=sample({});
 const double initial=c::deposit(q,s).z;
 check(initial>.03&&initial<.04,"strength");
 for(int i=0;i<24;++i){
  q.start=q.end=s.position;
  const auto v=c::deposit(q,s);
  check(std::abs(v.z-initial)<1e-12,"dabs add without stroke ceiling");
  s.position+=v;
 }
 check(s.position.z>.70,"no old .30R plateau");
 q.start=q.end={};
 check(c::deposit(q,sample({.939,0,0})).z<1e-6,"soft outer rim");
 check(d::norm2(c::deposit(q,sample({},0.)))==0.,"mask");
 check(d::norm2(c::deposit(q,sample({},1.,{0,0,-1})))==0.,"backface");
 q.dose=0.;check(d::norm2(c::deposit(q,sample({})))==0.,"zero dose");
 q.dose=.10;q.start={-.05,0,0};q.end={.05,0,0};
 const auto one=c::deposit(q,sample({.1,.2,0}));
 auto left=q;left.end={};left.dose=.05;
 auto right=q;right.start={};right.dose=.05;
 check(d::norm(one-c::deposit(left,sample({.1,.2,0}))-c::deposit(right,sample({.1,.2,0})))<1e-12,
       "partitioned round arc dose");
 m::Settings k;k.mobility=.85;k.old_limit=.035;k.relaxation=.75;
 f::Stats stats;
 std::vector<PhysicalBrushIntentSample> patch{sample({0,0,.08}),sample({.05,0,0}),sample({.10,0,0},0.),sample({.05,.05,0})};
 std::vector<Vec3> motion{{0,0,.06},{},{},{}};
 check(f::apply(k,patch,{{0,1},{1,2}},{1.,2.,1.,1.},{.06,.06,.06,.06},motion,nullptr,&stats),"collar exchange");
 check(stats.collar_writes==1&&motion[1].z>0.&&motion[2].z==0.&&motion[3].z==0.,"zero raw deposit collar and permissions");
 check(std::abs(motion[0].z+2*motion[1].z-.06)<1e-12,"axial balance");
 motion={{},{0,0,.05},{},{}};
 check(f::apply(k,patch,{{0,1},{1,2}},{1.,2.,1.,1.},{.06,.06,.06,.06},motion,nullptr,&stats),"old ridge");
 check(stats.old_donors==1&&motion[0].z<0.&&motion[1].z>.05,"old surface mobilised");
 check(std::abs(motion[0].z+2*motion[1].z-.10)<1e-12,"old ridge balance");
 motion.assign(4,{});
 check(f::apply(k,patch,{{0,1}},{1.,2.,1.,1.},{.06,.06,.06,.06},motion,nullptr,&stats)&&stats.links==0,
       "no spontaneous relaxation");
 std::puts("D14_CONTINUOUS_FUSION_REFERENCE_OK");
}
