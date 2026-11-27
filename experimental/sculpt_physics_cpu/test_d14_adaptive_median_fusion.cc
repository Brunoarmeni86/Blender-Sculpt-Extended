/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d14_adaptive_median_fusion.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics;
namespace m=dedicated_d14::median_fusion;
static void check(bool b,const char* s){if(!b){std::fprintf(stderr,"median fusion: %s\n",s);std::abort();}}
static PhysicalBrushIntentSample sample(Vec3 p,double w=1.,Vec3 n={0,0,1}){
 return {0,p,w,1.,true,n};
}
int main(){
 m::Settings k;
 m::Statistics stats;
 std::vector<PhysicalBrushIntentSample> s{sample({0,0,.2}),sample({.1,0,0}),sample({.2,0,0},0.)};
 std::vector<Vec3> motion{{0,0,.005},{0,0,.04},{}};
 check(m::apply(k,s,{{0,1},{1,2}},{1.,2.,1.},{.1,.1,.1},motion,nullptr,&stats),"apply");
 check(stats.old_donors==1&&stats.old_mobilized>0.&&motion[0].z<0.&&motion[1].z>.04,"old material moves");
 check(std::abs(motion[0].z+2*motion[1].z-.085)<1e-12,"conservation");
 check(motion[2].z==0.&&std::abs(motion[0].z-.005)<=.1,"masked and cap");
 auto flat=s;flat[0].position.z=0.;
 motion={{0,0,.005},{0,0,.04},{}};
 check(m::apply(k,flat,{{0,1}},{1.,2.,1.},{.1,.1,.1},motion,nullptr,&stats),"flat");
 check(stats.links==0&&motion[0].z==.005&&motion[1].z==.04,"preserve profile on flat");
 auto slope=s; slope[0].position={0,0,.1};slope[1].position={.1,0,0};
 slope[0].surface_normal=slope[1].surface_normal={.7071067811865475,0,.7071067811865475};
 check(m::apply(k,slope,{{0,1}},{1.,2.,1.},{.1,.1,.1},motion,nullptr,&stats)&&stats.links==0,"tangent detrending");
 motion={{},{},{}};
 check(m::apply(k,s,{{0,1}},{1.,2.,1.},{.1,.1,.1},motion,nullptr,&stats)&&stats.links==0,"no spontaneous motion");
 motion={{0,0,.005},{0,0,.04},{}};
 check(m::apply(k,s,{{0,1}},{1.,2.,1.},{0.,.1,.1},motion,nullptr,&stats)&&stats.links==0,"no headroom");
 auto back=s;back[1].surface_normal={0,0,-1};
 check(m::apply(k,back,{{0,1}},{1.,2.,1.},{.1,.1,.1},motion,nullptr,&stats)&&stats.links==0,"back sheet");
 check(!m::apply(k,s,{{0,5}},{1.,2.,1.},{.1,.1,.1},motion),"invalid edge");
 check(motion[0].z==.005&&motion[1].z==.04,"invalid patch transactional");
 std::puts("D14_ADAPTIVE_MEDIAN_FUSION_REFERENCE_OK");
}
