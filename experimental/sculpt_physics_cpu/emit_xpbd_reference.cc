/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A deterministic one-dab C++ World fixture for Python/Warp numerical parity.
 * This program emits ONLY final positions (one x y z line per particle),
 * and must not be treated as a native Blender viewport/Multires test.
 */
#include "physics_core.hh"

#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace sculpt_physics;

int main()
{
  World world;
  world.add_particle({0,0,0},0.0); // pinned (outside editable PBVH halo)
  world.add_particle({1,0,0},1.0);
  world.add_particle({2,0,0},1.0);
  world.add_bond(0,1,0.0);
  world.add_bond(1,2,1.0e-5);
  world.begin_stroke();
  if (!world.set_brush_targets({{2,{2,0.8,0},1.0e-4}})) {
    throw std::runtime_error("Could not create deterministic XPBD target");
  }
  // One substep, exactly the same constraint order and no initial velocity.
  // A 2-hop region covers both editable particles and the fixed exterior.
  world.step_local(1.0/60.0,1,12,2);
  std::cout<<std::setprecision(17);
  for (const Particle &p : world.particles()) {
    std::cout<<p.position.x<<" "<<p.position.y<<" "<<p.position.z<<"\n";
  }
  world.cancel_stroke();
  return 0;
}
