/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sculpt_surface_bridge.hh"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace sculpt_physics;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("FAILED: " #c); } while (false)

int main()
{
  // Collinear rest anchors use an object-space residual fallback. This is
  // allowed for existing reference input but must NEVER emit NaN coordinates.
  std::vector<Particle> rest{
    {{0,0,0},{},1,.05},{{1,0,0},{},1,.05},{{2,0,0},{},1,.05}};
  SurfaceAnchor anchor;
  anchor.vertices[0]=0; anchor.vertices[1]=1; anchor.vertices[2]=2;
  anchor.weights[0]=.25; anchor.weights[1]=.5; anchor.weights[2]=.25;
  auto bridge=SculptSurfaceBridge::for_multires_samples({{1,0,.1}},{anchor},rest);
  auto current=rest;
  current[1].position.x=std::numeric_limits<double>::quiet_NaN();
  bool rejected=false;
  try {
    (void)bridge.compose_positions(current,{{1,0,.1}},
                                    {BrushPhysicsMode::FullPhysics,1,0});
  }
  catch(const std::invalid_argument &) { rejected=true; }
  CHECK(rejected);
  current=rest;
  rejected=false;
  try {
    (void)bridge.compose_positions(current,{{NAN,0,.1}},
                                    {BrushPhysicsMode::PhysicalAssist,.5,0});
  }
  catch(const std::invalid_argument &) { rejected=true; }
  CHECK(rejected);
  // Original brush-only code path retains its exact bypass semantics.
  auto out=bridge.compose_positions(current,{{1,0,.1}},
                                     {BrushPhysicsMode::Standard,1,0});
  CHECK(out.size()==1 && out[0].z==.1);
  std::cout<<"Multires reconstructed output finite-safety tests passed\n";
}
