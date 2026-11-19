/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Accuracy diagnostic on a connected mesh; not Blender viewport timing. */
#include "physics_core.hh"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
using namespace sculpt_physics;
static World make_grid(double compliance)
{
  World w;
  constexpr int n=25;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x)
    w.add_particle({double(x),double(y),0});
  for(int y=0;y<n;++y)for(int x=0;x<n;++x){
    const int i=y*n+x;
    if(x+1<n)w.add_bond(i,i+1,compliance);
    if(y+1<n)w.add_bond(i,i+n,compliance);
  }
  return w;
}
int main()
{
  std::cout<<"compliance,mode,max_position_error,used_global_fallback,converged\n";
  for(double c : {0.0,1e-10,1e-6,1e-3}) {
    World ref=make_grid(c);
    World local4=ref,local8=ref,strict=ref,bounded=ref;
    for(World *w : {&ref,&local4,&local8,&strict,&bounded}) {
      w->begin_stroke();
      w->apply_grab({12,12,0},1.2,{.8,.4,0},
                    {BrushPhysicsMode::FullPhysics,1,0});
    }
    ref.step(1./30.,4,5);
    local4.step_local(1./30.,4,5,4);
    local8.step_local(1./30.,4,5,8);
    const auto strict_result=strict.step_local_adaptive(1./30.,4,5,4,8,.02);
    const auto bounded_result=bounded.step_local_adaptive(
      1./30.,4,5,4,8,.02,World::BoundaryPolicy::BoundedApproximation);
    auto report=[&](const char *mode,const World &w,bool fallback,bool converged) {
      double largest_error=0;
      for(std::size_t i=0;i<w.particles().size();++i) {
        const Vec3 &a=w.particles()[i].position,&b=ref.particles()[i].position;
        largest_error=std::max(largest_error,
          std::hypot(a.x-b.x,a.y-b.y));
      }
      std::cout<<c<<','<<mode<<','<<largest_error<<','
               <<fallback<<','<<converged<<'\n';
    };
    report("local_4",local4,false,false);
    report("local_8",local8,false,false);
    report("adaptive_strict",strict,strict_result.used_global_fallback,strict_result.converged);
    report("adaptive_bounded",bounded,bounded_result.used_global_fallback,bounded_result.converged);
  }
}
