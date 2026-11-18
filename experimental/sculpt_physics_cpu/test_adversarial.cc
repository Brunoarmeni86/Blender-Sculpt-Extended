/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include "mask_extract_core.hh"
#include "sculpt_surface_bridge.hh"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace sculpt_physics;
static void check(bool ok, const char *message)
{
  if (!ok) throw std::runtime_error(message);
}

int main()
{
  // A numerically fixed outer halo is not a physical material clamp:
  // do not plastically deform or break bonds crossing that boundary.
  {
    World w;
    w.add_particle({0, 0, 0});
    w.add_particle({1, 0, 0});
    const auto b = w.add_bond(0, 1, 0, 0.01, 100.0,
                              std::numeric_limits<double>::infinity());
    w.begin_stroke();
    w.apply_grab({0, 0, 0}, 0.1, {0, 1, 0},
                 {BrushPhysicsMode::PhysicalAssist, 1, 0});
    w.step_local(1.0 / 30.0, 1, 1, 0);
    check(std::abs(w.bonds()[b].rest_length - 1.0) < 1e-12,
          "Artificial local boundary changed permanent plastic rest length");
    w.cancel_stroke();
  }
  {
    World w;
    w.add_particle({0, 0, 0});
    w.add_particle({1, 0, 0});
    const auto b = w.add_bond(0, 1, 0,
                             std::numeric_limits<double>::infinity(), 0, 0.05);
    w.begin_stroke();
    w.apply_grab({0, 0, 0}, 0.1, {0, 1, 0},
                 {BrushPhysicsMode::PhysicalAssist, 1, 0});
    w.step_local(1.0 / 30.0, 1, 1, 0);
    check(!w.bonds()[b].broken, "Artificial local boundary fractured bond");
    w.cancel_stroke();
  }
  // Both ends of an internal material bond remain eligible for fracture.
  {
    World w;
    w.add_particle({0, 0, 0});
    w.add_particle({1, 0, 0});
    const auto b = w.add_bond(0, 1, 0,
                             std::numeric_limits<double>::infinity(), 0, 0.05);
    w.begin_stroke();
    w.apply_grab({0, 0, 0}, 0.1, {0, 1, 0},
                 {BrushPhysicsMode::PhysicalAssist, 1, 0});
    w.step_local(1.0 / 30.0, 1, 1, 1);
    check(w.bonds()[b].broken, "Internal fracture unexpectedly disabled");
    w.cancel_stroke();
  }
  // Finite coordinates may still overflow when computing a bond length.
  {
    World w;
    w.add_particle({-1e308, 0, 0});
    w.add_particle({1e308, 0, 0});
    bool rejected = false;
    try { (void)w.add_bond(0, 1, 0); }
    catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Infinite rest bond length accepted");
  }
  // Non-finite per-corner texture data is invalid. Real UDIM values >1 are OK.
  {
    TriangleMesh m;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    m.mask = {1, 1, 1};
    SurfaceTriangle f;
    f.vertices = {0, 1, 2};
    f.uv[1].u = std::numeric_limits<double>::quiet_NaN();
    m.faces = {f};
    bool rejected = false;
    try { (void)extract_mask_high_fidelity(m, 0.5); }
    catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Non-finite UV accepted by mask extractor");
    m.faces[0].uv[1].u = 3.0;
    check(extract_mask_high_fidelity(m, 0.5).faces.size() == 1,
          "Valid multi-tile UV improperly rejected");
  }
  // Proxy alignment must not become permissive as object coordinates grow.
  {
    World w;
    w.add_particle({1e12 + 100.0, 0, 0});
    bool rejected = false;
    try {
      (void)SculptSurfaceBridge::for_regular_mesh({{1e12, 0, 0}}, w.particles());
    }
    catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "Grossly shifted regular Sculpt proxy accepted");
  }
  std::cout << "Adversarial CPU physics and extraction tests passed\n";
}
