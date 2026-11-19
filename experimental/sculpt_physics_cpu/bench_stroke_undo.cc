/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone wall-clock CPU stroke benchmark (no Blender PBVH/viewport). */
#include "physics_core.hh"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>
using namespace sculpt_physics;
using Clock = std::chrono::steady_clock;
static double diff(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double,std::milli>(b-a).count();
}
static double median(std::vector<double> v) {
  std::sort(v.begin(),v.end());return v[v.size()/2];
}
int main() {
  for (int n : {10000,50000,125000}) {
    World w;
    for(int i=0;i<n;++i)w.add_particle({double(i),0,0});
    for(int i=0;i<n-1;++i)w.add_bond(i,i+1,1.e-5);
    std::vector<double> empty,local,global;
    for(int k=0;k<9;++k) {
      auto t0=Clock::now();w.begin_stroke();auto t1=Clock::now();w.commit_stroke();
      if(k)empty.push_back(diff(t0,t1));
      t0=Clock::now();w.begin_stroke();
      w.apply_grab({0,0,0},.25,{0,.1,0},{BrushPhysicsMode::FullPhysics,1,0});
      w.step_local(1./30.,1,2,2);
      t1=Clock::now();w.cancel_stroke();
      if(k)local.push_back(diff(t0,t1));
    }
    if(n<=50000)for(int k=0;k<6;++k){
      auto a=Clock::now();w.begin_stroke();
      w.apply_grab({0,0,0},.25,{0,.1,0},{BrushPhysicsMode::FullPhysics,1,0});
      w.step(1./30.,1,2);auto b=Clock::now();w.cancel_stroke();
      if(k)global.push_back(diff(a,b));
    }
    std::cout << "particles="<<n<<" bonds="<<w.bonds().size()<<" begin_ms="
              <<std::fixed<<std::setprecision(4)<<median(empty)
              <<" local_stroke_ms="<<median(local);
    if(!global.empty())std::cout<<" global_stroke_ms="<<median(global);
    std::cout<<'\n';
  }
}
