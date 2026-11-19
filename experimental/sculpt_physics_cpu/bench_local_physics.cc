/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone CPU benchmark: compare optional local proxy physics to global.
 * Not a benchmark for Blender PBVH/Sculpt Mode or a fidelity validation. */
#include "physics_core.hh"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
using namespace sculpt_physics;
using Clock = std::chrono::steady_clock;
namespace {
World make_grid(int n)
{
  World w;
  const double spacing = 0.075;
  const auto index = [n](int x, int y, int z) {
    return std::size_t((z * n + y) * n + x);
  };
  for (int z=0;z<n;z++) for (int y=0;y<n;y++) for (int x=0;x<n;x++) {
    w.add_particle({x*spacing,y*spacing,z*spacing});
  }
  for (int z=0;z<n;z++) for (int y=0;y<n;y++) for (int x=0;x<n;x++) {
    if (x+1<n) w.add_bond(index(x,y,z),index(x+1,y,z),1e-5);
    if (y+1<n) w.add_bond(index(x,y,z),index(x,y+1,z),1e-5);
    if (z+1<n) w.add_bond(index(x,y,z),index(x,y,z+1),1e-5);
  }
  return w;
}
struct Measure { double ms; std::size_t visited, particles, dirty; };
Measure run(World &w, int side, int hops)
{
  const double center = side * .075 / 2.0;
  w.begin_stroke();
  w.apply_grab({center,center,center},.20,{0,.1,0},
               {BrushPhysicsMode::PhysicalAssist,1,1e-6});
  auto a=Clock::now();
  if(hops < 0) w.step(1.0/30,4,5);
  else w.step_local(1.0/30,4,5,hops);
  auto b=Clock::now();
  auto stat=w.last_step_statistics();
  const auto dirty=w.dirty_particles().size();
  w.cancel_stroke();
  return {std::chrono::duration<double,std::milli>(b-a).count(),
          stat.visited_bonds,stat.integrated_particles,dirty};
}
}
int main()
{
  std::cout << "particles,bonds,mode,hops,median_solver_ms,integrated_particles,visited_bond_passes,dirty\n";
  for(const int side : {20,30,50}) {
    World w=make_grid(side);
    for(const int hops : {-1,2,4,8}) {
      (void)run(w,side,hops);
      std::vector<double> ms;
      Measure last{};
      for(int rep=0;rep<5;rep++) {
        last=run(w,side,hops);
        ms.push_back(last.ms);
      }
      std::sort(ms.begin(),ms.end());
      std::cout << w.particles().size() << ',' << w.bonds().size() << ','
                << (hops<0 ? "global" : "local") << ',' << hops << ','
                << std::fixed << std::setprecision(3) << ms[ms.size()/2] << ','
                << last.particles << ',' << last.visited << ',' << last.dirty << '\n';
    }
  }
}
