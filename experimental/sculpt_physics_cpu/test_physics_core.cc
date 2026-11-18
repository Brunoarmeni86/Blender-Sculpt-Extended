/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include "mask_extract_core.hh"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace sculpt_physics;

#define expect(condition) \
  do { \
    if (!(condition)) throw std::runtime_error("FAILED: " #condition); \
  } while (false)


bool near(double a, double b, double eps = 1e-7)
{
  return std::abs(a - b) < eps;
}

int main()
{
  // OFF/0% physics must not change the existing brush path.
  {
    World w;
    w.add_particle({0, 0, 0});
    w.begin_stroke();
    bool changed = w.apply_grab({0, 0, 0}, 2, {1, 0, 0},
                                {BrushPhysicsMode::Standard, 1.0, 0.0});
    expect(!changed && near(w.particles()[0].position.x, 0));
    changed = w.apply_grab({0, 0, 0}, 2, {1, 0, 0},
                           {BrushPhysicsMode::PhysicalAssist, 0.0, 0.0});
    expect(!changed && near(w.particles()[0].position.x, 0));
    w.cancel_stroke();
  }
  // Grab moves a local particle while a connected neighbor follows through XPBD.
  {
    World w;
    w.add_particle({0, 0, 0});
    w.add_particle({1, 0, 0});
    w.add_particle({10, 0, 0});
    w.add_bond(0, 1, 1e-5);
    w.begin_stroke();
    expect(w.apply_grab({0, 0, 0}, 0.8, {0, 1, 0},
                        {BrushPhysicsMode::PhysicalAssist, 1, 1e-7}));
    w.step(1.0 / 30, 4, 8);
    expect(w.particles()[0].position.y > 0.1);
    expect(w.particles()[1].position.y > 0.0);
    expect(near(w.particles()[2].position.y, 0.0));
    expect(!w.dirty_particles().empty());
    w.cancel_stroke();
    expect(near(w.particles()[0].position.y, 0.0));
    expect(near(w.particles()[1].position.y, 0.0));
    expect(w.dirty_particles().empty());
  }
  // A stretched bond can plastically lengthen or fracture.
  {
    World w;
    w.add_particle({0, 0, 0}, 0);
    w.add_particle({1, 0, 0});
    const auto bond_id = w.add_bond(0, 1, 0.0, 0.01, 30.0, 100.0);
    w.begin_stroke();
    w.apply_grab({1, 0, 0}, 0.8, {1, 0, 0},
                 {BrushPhysicsMode::FullPhysics, 1, 0});
    w.step(0.1, 2, 4);
    expect(w.bonds()[bond_id].rest_length > 1.0);
    w.cancel_stroke();
    expect(near(w.bonds()[bond_id].rest_length, 1.0));
  }
  {
    World w;
    w.add_particle({0, 0, 0}, 0);
    w.add_particle({1, 0, 0});
    const auto id = w.add_bond(0, 1, 0, std::numeric_limits<double>::infinity(), 0, 0.05);
    w.begin_stroke();
    w.apply_grab({1, 0, 0}, 0.7, {1, 0, 0},
                 {BrushPhysicsMode::FullPhysics, 1, 0});
    w.step(0.1, 1, 3);
    expect(w.bonds()[id].broken);
    w.cancel_stroke();
    expect(!w.bonds()[id].broken);
  }
  // UV seams are corner data, not a shared per-particle UV.
  {
    World w;
    w.add_particle({0, 0, 0});
    w.add_particle({1, 0, 0});
    w.add_particle({0, 1, 0});
    const SurfaceAnchor side_a{{0, 1, 2}, {0.25, 0.25, 0.5}, {{0,0}, {1,0}, {0,1}}};
    const SurfaceAnchor side_b{{0, 1, 2}, {0.25, 0.25, 0.5}, {{2,0}, {3,0}, {2,1}}};
    const auto a = evaluate_anchor(side_a, w.particles());
    const auto b = evaluate_anchor(side_b, w.particles());
    expect(near(a.position.x, b.position.x));
    expect(near(a.uv.u + 2.0, b.uv.u));
  }
  // CPU no-gravity ground contact; pinned particle must not move.
  {
    World w;
    w.add_particle({0, 0.1, 0}, 0.0, 0.1);
    w.add_particle({1, 0.1, 0}, 1.0, 0.1);
    w.set_gravity({0, -9.81, 0});
    w.set_ground_height(0);
    w.begin_stroke();
    w.step(0.5, 8, 5);
    expect(near(w.particles()[0].position.y, 0.1));
    expect(w.particles()[1].position.y >= 0.1 - 1e-9);
    w.commit_stroke();
  }
  // HR mask triangle clipping with welded cut boundaries and corner-domain UV seams.
  {
    TriangleMesh src;
    src.positions = {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}};
    src.mask = {0.0, 1.0, 1.0, 0.0};
    SurfaceTriangle f0;
    f0.vertices = {0,1,2};
    f0.uv = {{{0,0}, {1,0}, {1,1}}};
    f0.colors = {{{0,0,0,1}, {1,0,0,1}, {1,1,0,1}}};
    f0.material_id = 7;
    SurfaceTriangle f1;
    f1.vertices = {0,2,3};
    f1.uv = {{{2,0}, {3,1}, {2,1}}};
    f1.colors = {{{0,0,1,1}, {0,1,0,1}, {1,0,1,1}}};
    f1.material_id = 9;
    src.faces = {f0, f1};
    auto out = extract_mask_high_fidelity(src, 0.5);
    expect(src.positions.size() == 4 && src.faces.size() == 2);
    expect(out.faces.size() == 3);
    expect(out.positions.size() == 5); // Cut at source edges 0-1, 0-2, 2-3.
    for (const auto &p : out.positions) expect(p.x >= 0.5 - 1e-9);
    bool seen_left_uv = false, seen_right_uv = false, seen_material7 = false;
    for (const auto &f : out.faces) {
      seen_material7 |= f.material_id == 7;
      for (int k = 0; k < 3; ++k) {
        const auto &p = out.positions[f.vertices[k]];
        if (near(p.x, 0.5) && near(p.y, 0.5)) {
          if (near(f.uv[k].u, 0.5)) seen_left_uv = true;
          if (near(f.uv[k].u, 2.5)) seen_right_uv = true;
        }
      }
    }
    expect(seen_left_uv && seen_right_uv && seen_material7);
  }
  std::cout << "All Sculpt Physics CPU reference tests passed\n";
}
