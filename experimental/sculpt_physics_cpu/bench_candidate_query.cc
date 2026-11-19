/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Preselected vertex-index performance diagnostic (not PBVH query cost). */
#include "physics_core.hh"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>
using namespace sculpt_physics;
using Clock=std::chrono::steady_clock;

int main()
{
  std::cout<<"particles,full_scan_ms,indexed_3_ms\n";
  for(int n:{10000,125000,500000}) {
    World w;
    for(int i=0;i<n;++i) w.add_particle({double(i)*.1,0,0});
    w.begin_stroke();
    const BrushPhysicsSettings cfg{BrushPhysicsMode::FullPhysics,1,1e-6};
    const std::vector<std::size_t> candidates{11,10,9};
    double full=0,indexed=0;
    for(int rep=0;rep<200;++rep) {
      const auto a=Clock::now();
      w.apply_grab({1,0,0},.15,{0,.2,0},cfg);
      const auto b=Clock::now();
      w.apply_grab_candidates({1,0,0},.15,{0,.2,0},cfg,candidates);
      const auto c=Clock::now();
      if(rep>=10) {
        full+=std::chrono::duration<double,std::milli>(b-a).count();
        indexed+=std::chrono::duration<double,std::milli>(c-b).count();
      }
    }
    std::cout<<n<<','<<std::fixed<<std::setprecision(6)
             <<full/190<<','<<indexed/190<<'\n';
    w.cancel_stroke();
  }
}
