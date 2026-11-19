/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "physics_core.hh"
#include "sculpt_surface_bridge.hh"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("FAILED: " #x); } while (false)
static bool near(double a, double b, double eps=1e-7) { return std::abs(a-b)<eps; }
int main()
{
  // Regular mesh Sculpt with NO Multires modifier: one-to-one vertex mappings.
  World w;
  std::vector<Vec3> original{{0,0,0}, {1,0,0}, {2,0,0}};
  for (const Vec3 &v : original) w.add_particle(v);
  w.add_bond(0, 1, 0);
  w.add_bond(1, 2, 0);
  auto bridge = SculptSurfaceBridge::for_regular_mesh(original, w.particles());
  CHECK(bridge.kind() == SculptSurfaceKind::RegularMesh);
  CHECK(bridge.sample_count() == 3);
  w.begin_stroke();
  CHECK(w.apply_grab({0,0,0}, 0.5, {0,1,0},
                     {BrushPhysicsMode::PhysicalAssist,1,0}));
  w.step_local(1.0/30.0, 1, 4, 1);
  CHECK(w.last_step_statistics().integrated_particles == 2);
  CHECK(w.last_step_statistics().visited_bonds == 8);
  CHECK(!w.last_step_statistics().used_global_fallback);
  CHECK(w.particles()[0].position.y > 0.5);
  CHECK(near(w.particles()[2].position.y, 0.0)); // beyond local halo
  const std::vector<Vec3> artistic{{0,0.25,0}, {1,0,0}, {2,0,0}};
  const auto unchanged = bridge.compose_positions(w.particles(), artistic,
                                                   {BrushPhysicsMode::Standard,1,0});
  const auto off = bridge.compose_positions(w.particles(), artistic,
                                             {BrushPhysicsMode::PhysicalAssist,0,0});
  const auto assist = bridge.compose_positions(w.particles(), artistic,
                                                {BrushPhysicsMode::PhysicalAssist,0.5,0});
  const auto full = bridge.compose_positions(w.particles(), artistic,
                                              {BrushPhysicsMode::FullPhysics,1,0});
  for (std::size_t i=0;i<artistic.size();++i) {
    CHECK(near(unchanged[i].y, artistic[i].y));
    CHECK(near(off[i].y, artistic[i].y));
    CHECK(near(full[i].y, w.particles()[i].position.y));
    CHECK(near(assist[i].y, (artistic[i].y+full[i].y)*0.5));
  }
  CHECK(near(original[0].y, 0)); // bridge doesn't mutate the source
  w.cancel_stroke();
  CHECK(near(w.particles()[0].position.y, 0));

  // Multires sampling: coarse proxy deformation + preserved high-frequency
  // residual. The same physics core is used; the bridge differs only.
  World proxy;
  proxy.add_particle({0,0,0});
  proxy.add_particle({1,0,0});
  proxy.add_particle({0,1,0});
  SurfaceAnchor anchor{{0,1,2}, {0.25,0.25,0.5}, {{0,0},{1,0},{0,1}}};
  std::vector<Vec3> high{{0.25,0.5,0.2}}; // fine sculpt detail z=+0.2
  auto high_bridge = SculptSurfaceBridge::for_multires_samples(
      high, {anchor}, proxy.particles());
  CHECK(high_bridge.kind() == SculptSurfaceKind::MultiresSamples);
  proxy.begin_stroke();
  proxy.apply_grab({0,0,0}, 2.0, {0,0,1}, {BrushPhysicsMode::FullPhysics,1,0});
  proxy.step_local(1.0/30, 1, 2, 1);
  const auto raised = high_bridge.compose_positions(
      proxy.particles(), high, {BrushPhysicsMode::FullPhysics,1,0});
  const double proxy_z = 0.25*proxy.particles()[0].position.z +
                          0.25*proxy.particles()[1].position.z +
                          0.5*proxy.particles()[2].position.z;
  // Detail magnitude is preserved in the moving proxy frame; its normal
  // may rotate so object-space Z is no longer a valid expected component.
  const Vec3 raised_delta = raised[0] - Vec3{0.25, 0.5, proxy_z};
  CHECK(near(std::sqrt(raised_delta.x*raised_delta.x +
                       raised_delta.y*raised_delta.y +
                       raised_delta.z*raised_delta.z), 0.2));
  CHECK(near(high[0].z, 0.2));
  proxy.cancel_stroke();

  // Graph-halo locality: only an island and its boundary bonds are computed.
  World chain;
  for (int i=0;i<100;++i) chain.add_particle({double(i),0,0});
  for (int i=0;i<99;++i) chain.add_bond(i,i+1,0);
  chain.begin_stroke();
  chain.apply_grab({50,0,0}, .25, {0,1,0},
                   {BrushPhysicsMode::PhysicalAssist,1,0});
  chain.step_local(1.0/30,1,10,3);
  CHECK(chain.last_step_statistics().integrated_particles == 7);
  CHECK(chain.last_step_statistics().visited_bonds == 80);
  CHECK(near(chain.particles()[10].position.y,0));
  CHECK(chain.particles()[50].position.y>0);
  chain.cancel_stroke();
  // Any global force must cause explicit full-world fallback (correctness).
  chain.set_gravity({0,-9.81,0});
  chain.begin_stroke();
  chain.apply_grab({50,0,0}, .25, {0,1,0},
                   {BrushPhysicsMode::PhysicalAssist,1,0});
  chain.step_local(1.0/30,1,1,2);
  CHECK(chain.last_step_statistics().used_global_fallback);
  CHECK(chain.last_step_statistics().integrated_particles == 100);
  chain.cancel_stroke();
  // Local sculpt with no selected particles must remain idle; do not wake the whole world.
  {
    World w;
    for (int i=0; i<3; ++i) w.add_particle({double(i),0,0});
    for (int i=0; i<2; ++i) w.add_bond(i,i+1,1e-5);
    w.begin_stroke();
    CHECK(w.apply_grab({0,0,0},0.4,{0,1,0},
                       {BrushPhysicsMode::PhysicalAssist,1,0}));
    w.step_local(1./30,1,3,1);
    const auto before=w.particles();
    CHECK(!w.apply_grab({100,0,0},0.2,{0,1,0},
                        {BrushPhysicsMode::PhysicalAssist,1,0}));
    w.step_local(1./30,1,3,1);
    for (int i=0; i<3; ++i) CHECK(near(w.particles()[i].position.y,before[i].position.y));
    CHECK(w.dirty_particles().empty());
    w.cancel_stroke();
  }
  // A normal-mesh proxy must start in the same space, or its first active dab jumps.
  {
    World w;
    w.add_particle({100,0,0});
    bool rejected=false;
    try { (void)SculptSurfaceBridge::for_regular_mesh({{0,0,0}},w.particles()); }
    catch(const std::invalid_argument &) { rejected=true; }
    CHECK(rejected);
  }
  // Multires detail must rotate with the coarse surface frame, not stay in world Z.
  {
    World rest;
    rest.add_particle({0,0,0}); rest.add_particle({1,0,0}); rest.add_particle({0,1,0});
    SurfaceAnchor anchor{{0,1,2},{.25,.25,.5},{{0,0},{1,0},{0,1}}};
    auto bridge=SculptSurfaceBridge::for_multires_samples({{.25,.5,.2}}, {anchor}, rest.particles());
    std::vector<Particle> rotated=rest.particles();
    // Rotation 90 degrees about X: original +Z should become -Y.
    for (auto &p:rotated) {const auto q=p.position; p.position={q.x,-q.z,q.y};}
    auto out=bridge.compose_positions(rotated,{{.25,.5,.2}}, {BrushPhysicsMode::FullPhysics,1,0});
    CHECK(near(out[0].y,-.2));
    CHECK(near(out[0].z,.5));
  }
  std::cout << "Regular Sculpt + Multires surface bridge and local solver tests passed\n";
}
