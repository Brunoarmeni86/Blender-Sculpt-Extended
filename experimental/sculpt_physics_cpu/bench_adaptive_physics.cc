/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone benchmark, not a Blender viewport performance claim. */
#include "physics_core.hh"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace sculpt_physics;
using Clock=std::chrono::steady_clock;
namespace {
World make_grid(const int n)
{
  World w;
  const double spacing=.075;
  auto id=[&](int x,int y,int z){return std::size_t((z*n+y)*n+x);};
  for(int z=0;z<n;++z)for(int y=0;y<n;++y)for(int x=0;x<n;++x)
    w.add_particle({x*spacing,y*spacing,z*spacing});
  for(int z=0;z<n;++z)for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
    if(x+1<n)w.add_bond(id(x,y,z),id(x+1,y,z),1e-5);
    if(y+1<n)w.add_bond(id(x,y,z),id(x,y+1,z),1e-5);
    if(z+1<n)w.add_bond(id(x,y,z),id(x,y,z+1),1e-5);
  }
  return w;
}
struct Result {double ms;int hops;int attempts;bool fallback;size_t integrated;};
Result run(World &w,int n,int mode)
{
  const double c=n*.075/2;
  w.begin_stroke();
  w.apply_grab({c,c,c},.2,{0,.1,0},
               {BrushPhysicsMode::FullPhysics,1,1.e-6});
  const auto a=Clock::now();
  World::AdaptiveStepResult adap{};
  if(mode==-1)w.step(1./30.,4,5);
  else if(mode<10)w.step_local(1./30.,4,5,mode);
  else adap=w.step_local_adaptive(
      1./30.,4,5,4,(mode==10||mode==11)?8:16,.02,
      (mode==11||mode==17)?World::BoundaryPolicy::BoundedApproximation:
                            World::BoundaryPolicy::StrictGlobalFallback);
  const auto b=Clock::now();
  const auto integrated=w.last_step_statistics().integrated_particles;
  w.cancel_stroke();
  return{std::chrono::duration<double,std::milli>(b-a).count(),
         adap.accepted_hops,adap.attempts,adap.used_global_fallback,integrated};
}
} // namespace
int main()
{
  std::cout<<"particles,mode,median_ms,accepted_hops,attempts,fallback,integrated\n";
  for(const int n:{20,30,50}) {
    World w=make_grid(n);
    // 4/8 = fixed-hop; 10 = strict max8; 11 = bounded max8;
    // 16 = strict max16; 17 = bounded max16; -1 = full global.
    for(const int mode:{4,8,10,11,16,17,-1}) {
      run(w,n,mode); // warmup
      std::vector<double> samples;
      Result r{};
      for(int i=0;i<5;++i) {
        r=run(w,n,mode);
        samples.push_back(r.ms);
      }
      std::sort(samples.begin(),samples.end());
      std::cout<<w.particles().size()<<','<<mode<<','
               <<std::fixed<<std::setprecision(3)<<samples[2]<<','
               <<r.hops<<','<<r.attempts<<','<<r.fallback<<','
               <<r.integrated<<'\n';
    }
  }
}
