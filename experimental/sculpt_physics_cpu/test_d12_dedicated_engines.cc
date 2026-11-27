/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d12_clay_engine.hh"
#include "d12_flesh_engine.hh"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace sculpt_physics;
using namespace sculpt_physics::dedicated_d12;
static void check(bool b,const char* name){if(!b){std::fprintf(stderr,"D12 FAILED: %s\n",name);std::abort();}}
static PhysicalBrushIntentSample sample(Vec3 p,double w=1.,Vec3 n={0,0,1},bool write=true){
 return {0,p,w,1.,write,n};
}
int main(){
 ClayParams p;
 Frame frame{true,{0,0,0},{0,0,1},{1,0,0}};
 const auto kernel=prepare_clay(frame,1.,.1,1.,1.,p);
 auto center=sample({0,0,0});
 const auto v=clay_vertex(kernel,center);
 check(v.z>0&&v.z<p.offset,"unilateral deposition");
 check(norm2(clay_vertex(kernel,sample({0,0,.25})))==0,"above plane");
 check(norm2(clay_vertex(kernel,sample({0,0,-.8})))==0,"deep sheet");
 check(norm2(clay_vertex(kernel,sample({0,0,-.05},1.,{0,0,-1})))==0,"back sheet");
 check(norm2(clay_vertex(kernel,sample({0,0,0},0.)))==0,"mask");
 check(norm2(clay_vertex(kernel,sample({0,0,0},1.,{0,0,1},false)))==0,"permission");
 check(clay_vertex(kernel,sample({0,0,0},.5)).z<v.z,"fractional envelope");
 auto neg=prepare_clay(frame,1.,.1,1.,-1.,p);
 check(clay_vertex(neg,center).z<0,"invert");
 auto round=p;round.roundness=1.;
 auto square=p;square.roundness=0.;
 auto diagonal=sample({.70,.70,0});
 check(clay_vertex(prepare_clay(frame,1.,.1,1.,1.,square),diagonal).z>
       clay_vertex(prepare_clay(frame,1.,.1,1.,1.,round),diagonal).z,"tip shape");
 for(int n:{1,2,8,64}){
  auto q=center;
  const auto step=prepare_clay(frame,1.,.1/double(n),1.,1.,p);
  for(int j=0;j<n;++j)q.position+=clay_vertex(step,q);
  check(std::abs(q.position.z-v.z)<1e-12,"fixed-plane dose partition");
 }
 std::vector<PhysicalBrushIntentSample> samples;
 for(int y=-50;y<=50;++y)for(int x=-50;x<=50;++x)
  samples.push_back(sample({x*.02,y*.02,0}));
 auto f=clay_frame(samples,{0,0,0},{0,0,1},{.05,0,0},1.,{},p);
 check(f.valid&&dot(f.axis,{0,0,1})>.999,"live frame");
 // A large discontinuity resets orientation memory.
 auto old=Frame{true,{0,0,0},{1,0,0},{0,1,0}};
 auto next=clay_frame(samples,{0,0,0},{0,0,1},{2.,0,0},1.,old,p);
 check(dot(next.axis,{0,0,1})>.999,"gap reset");
 // Repeated advancing passes follow the changed live surface and build up.
 std::vector<PhysicalBrushIntentSample> live=samples;
 for(int step=0;step<12;++step){
  const Vec3 at={-.3+.05*step,0,0};
  auto local=clay_frame(live,at,{0,0,1},{.05,0,0},1.,{},p);
  auto k=prepare_clay(local,1.,.05,1.,1.,p);
  for(auto& s:live)s.position+=clay_vertex(k,s);
 }
 check(live[live.size()/2].position.z>v.z,"live buildup");
 // Reproduce the problematic positive feedback: the hit point follows
 // the current ridge upward on every dab. Bound cumulative motion even
 // when the moving plane itself never reaches equilibrium.
 auto rising=center;
 const ClayReference anchor{rising.position,rising.surface_normal,1.};
 for(int j=0;j<200;++j){
  const Frame live_frame{true,rising.position,{0,0,1},{1,0,0}};
  const auto k=prepare_clay(live_frame,1.,.12,2.,1.,p);
  rising.position+=clay_vertex(k,rising,&anchor);
  check(norm(rising.position-anchor.position)<=p.stroke_height+1e-12,"live cursor runaway");
 }
 check(rising.position.z>.15,"useful buildup retained");
 const ClayReference new_stroke{rising.position,rising.surface_normal,1.};
 const auto next_stroke=prepare_clay({true,rising.position,{0,0,1},{1,0,0}},1.,.10,1.,1.,p);
 check(clay_vertex(next_stroke,rising,&new_stroke).z>0,"new stroke can build further");
 // The target may be below the plane even at the edge: geometry support
 // must still fade continuously rather than move to a hard cutoff.
 const auto edge_near=clay_vertex(kernel,sample({.999,0,-.08}));
 check(norm(edge_near)<1e-5,"smooth outer rim");
 const auto deep_near=clay_vertex(kernel,sample({0,0,-p.max_depth+1e-5}));
 check(norm(deep_near)<1e-6,"smooth depth cutoff");
 const auto n=Vec3{std::sqrt(1.-.051*.051),0,.051};
 check(norm(clay_vertex(kernel,sample({0,0,0},1.,n)))<1e-5,"smooth facing cutoff");
 // Tilt of the live frame must not turn a previously deposited patch
 // into a tall tangential extrusion.
 auto tilted=prepare_clay({true,{0,0,0},unit({.7,0,1}),{0,1,0}},1.,.12,1.,1.,p);
 auto tilt_motion=clay_vertex(tilted,center,&anchor);
 check(tilt_motion.z>0&&std::abs(tilt_motion.x)<.4*tilt_motion.z,"original normal bias");
 FleshParams flesh;
 auto fk=prepare_flesh({0,0,0},{0,0,1},{.08,0,0},1.,1.,{},flesh);
 auto drag=flesh_vertex(fk,center);
 check(drag.x>0&&std::abs(drag.z)<1e-12,"tangent gesture");
 auto leading=flesh_vertex(fk,sample({.4,0,0}));
 auto trailing=flesh_vertex(fk,sample({-.4,0,0}));
 check(leading.z>0&&trailing.z<0,"paired bulge proxy");
 check(norm2(flesh_vertex(fk,sample({0,0,0},0.)))==0,"flesh pin");
 check(norm2(flesh_vertex(fk,sample({1.1,0,0})))==0,"flesh support");
 check(norm2(flesh_vertex(fk,sample({0,0,0},1.,{0,0,-1})))==0,"flesh back sheet");
 check(norm2(flesh_vertex(prepare_flesh({0,0,0},{0,0,1},{},1.,1.,fk.next,flesh),center))==0,"no stationary creep");
 check(norm2(flesh_vertex(prepare_flesh({0,0,0},{0,0,1},{2,0,0},1.,1.,fk.next,flesh),center))==0,"no gap jump");
 auto turn=prepare_flesh({0,0,0},{0,0,1},{0,.08,0},1.,1.,fk.next,flesh);
 check(turn.drag.x>0&&turn.drag.y>0,"directional lag");
 // Non-asserted timing, not a Blender benchmark. Keep the work observable.
 volatile double sink=0.;
 for(int count:{1024,4096,16384}){
  std::vector<PhysicalBrushIntentSample> work;
  work.reserve(count);
  for(int j=0;j<count;++j)work.push_back(samples[std::size_t(j)%samples.size()]);
  const auto t=std::chrono::steady_clock::now();
  for(int repeat=0;repeat<32;++repeat)
   for(const auto& s:work)sink=sink+clay_vertex(kernel,s).z;
  const double clay_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count()/32.;
  const auto u=std::chrono::steady_clock::now();
  for(int repeat=0;repeat<32;++repeat)
   for(const auto& s:work)sink=sink+flesh_vertex(fk,s).x;
  const double flesh_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-u).count()/32.;
  std::printf("D12 kernel-only vertices=%d clay_ms=%.6f flesh_ms=%.6f\n",count,clay_ms,flesh_ms);
 }
 check(std::isfinite(double(sink)),"timing checksum");
}
