/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "global_collision_v29.hh"
#include "d12_clay_engine.hh"
#include "artistic_paired_flux.hh"
#include "volume_session_v25.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics;
namespace d=dedicated_d12;
static void check(bool b,const char* s){if(!b){std::fprintf(stderr,"%s\n",s);std::abort();}}
int main(){
 std::array<Vec3,3> a{{{0,0,0},{2,0,0},{0,2,0}}},b{{{1.5,1.5,0},{3,1.5,0},{1.5,3,0}}};
 std::array<Vec3,3> zero{},move{{{-1,-1,0},{-1,-1,0},{-1,-1,0}}};
 double s=collision_v29::separating_scale(a,b,zero,move,0.,1.);
 check(s>0.&&s<.5,"coplanar separated triangles with crossing trajectory");
 check(collision_v29::separating_scale(a,b,zero,zero,0.,1.)==1.,"separated static triangles");
 check(collision_v29::separating_scale(a,a,zero,zero,0.,1.)==0.,"unresolved initial contact");
 d::ClayParams p;d::Frame frame{true,{0,0,0},{0,0,1},{1,0,0}};
 auto k=d::prepare_clay(frame,1.,.6,1.,1.,p);
 PhysicalBrushIntentSample sample{0,{-1.25,0,0},1.,1.,true,{0,0,1}};
 check(d::norm2(d::clay_vertex(k,sample))==0.,"endpoint outside");
 check(d::swept_clay_vertex(k,sample,{.6,0,0},nullptr).z>0.,"swept support");
 sample.writable=false;
 check(d::norm2(d::swept_clay_vertex(k,sample,{.6,0,0},nullptr))==0.,"permission");
 sample.writable=true;sample.surface_normal={0,0,-1};
 check(d::norm2(d::swept_clay_vertex(k,sample,{.6,0,0},nullptr))==0.,"back face");
 std::vector<double> delta;double moved=0;
 check(!paired_flux_dispatch(nullptr,{{0,1,.4}},{.2,0.},{0.,.3},delta,moved),"CPU backend");
 check(std::abs(delta[0]+delta[1])<1e-12&&std::abs(moved-.2)<1e-12,"conservation");
 bool rejected=false;try{paired_flux_dispatch(nullptr,{{0,2,.4}},{.2,0.},{0.,.3},delta,moved);}catch(const std::invalid_argument&){rejected=true;}
 check(rejected,"invalid index before backend");
 VolumeSessionV25 v;v.backend=VolumeBackendV25::LiquidPBF;v.particle_spacing=.1;v.liquid.push_back({{0,0,0},{}});
 VolumeFrameV25 step;step.gravity={0,0,0};step.tool_enabled=true;step.tool_start={5,0,0};step.tool_end={5.1,0,0};step.substeps=1;
 const auto after=stage_volume_adaptive_d13(v,step);
 check(v.time==0.&&after.time==step.dt&&after.liquid.size()==1,"atomic volume adaptive");
 std::puts("D13_COMBINED_REFERENCE_OK");
}
