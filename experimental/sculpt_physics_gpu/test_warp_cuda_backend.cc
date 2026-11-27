/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "brush_solver_backend.hh"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace sculpt_physics;

int main()
{
  const auto require = [](const bool condition, const char *message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  };
  BackendKind selected = BackendKind::Auto;
  std::unique_ptr<BrushSolverBackend> backend = create_brush_solver_backend(
      BackendKind::WarpCuda, &selected);
  require(bool(backend), "backend factory returned null");
  require(selected == BackendKind::WarpCuda, "Warp CUDA request fell back to CPU");
  require(backend->kind() == BackendKind::WarpCuda, "selected backend kind is not Warp CUDA");

  constexpr int side = 32;
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      const bool boundary = x == 0 || y == 0 || x == side - 1 || y == side - 1;
      backend->add_particle({double(x), double(y), 0.0}, boundary ? 0.0 : 1.0);
    }
  }
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      const std::size_t i = std::size_t(y * side + x);
      if (x + 1 < side) {
        backend->add_bond(i, i + 1, 1.0e-5);
      }
      if (y + 1 < side) {
        backend->add_bond(i, i + side, 1.0e-5);
      }
    }
  }
  backend->begin_stroke();
  const std::size_t center = std::size_t((side / 2) * side + side / 2);
  std::vector<std::size_t> active_region;
  for (int y = side / 2 - 3; y <= side / 2 + 3; y++) {
    for (int x = side / 2 - 3; x <= side / 2 + 3; x++) {
      active_region.push_back(std::size_t(y * side + x));
    }
  }
  require(backend->set_active_region(active_region), "Warp CUDA rejected a valid active region");
  require(backend->set_targets({{center, {double(side / 2), double(side / 2), 4.0}, 1.0e-6}}),
          "Warp CUDA rejected a valid target");
  backend->step_local(1.0 / 60.0, 3, 10, 0);
  const Vec3 result = backend->particles()[center].position;
  require(std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z),
          "Warp CUDA returned a non-finite position");
  require(result.z > 0.5, "Warp CUDA did not deform the target particle");
  const Vec3 neighbor = backend->particles()[center + 1].position;
  require(neighbor.z > 0.01, "Warp CUDA did not propagate into the active material halo");
  std::cout << "WARP_CUDA_BACKEND_OK center_z=" << result.z
            << " neighbor_z=" << neighbor.z << '\n';
  // Opt-in so historical Jacobi-only installations retain their existing test.
  // Run once per recovery policy in a fresh process, with matching A3 PTX.
  if (std::getenv("BLENDER_SCULPT_D13_SMOOTH_TEST")) {
    auto smooth = create_brush_solver_backend(BackendKind::WarpCuda,&selected);
    require(smooth && selected==BackendKind::WarpCuda,"Smooth CUDA unavailable");
    for(int y=0;y<side;++y)for(int x=0;x<side;++x) {
      const bool peak=x==side/2 && y==side/2;
      smooth->add_particle({double(x),double(y),peak?1.:0.},1.);
    }
    for(int y=0;y<side;++y)for(int x=0;x<side;++x){
      const std::size_t i=std::size_t(y*side+x);
      if(x+1<side)smooth->add_bond(i,i+1,0.);
      if(y+1<side)smooth->add_bond(i,i+side,0.);
    }
    smooth->begin_stroke();
    std::vector<float> weights(active_region.size(),.5f);
    std::vector<Vec3> normals(active_region.size(),Vec3{0,0,1});
    require(smooth->smooth_compact(active_region,weights,normals,
                                  SmoothSolverMode::DenseStrong,50.),
            "D13 dedicated Smooth failed; no Jacobi fallback allowed");
    const double z=smooth->particles()[center].position.z;
    require(std::isfinite(z)&&z>=0.&&z<.5,"D13 peak did not relax");
    require(smooth->particles()[0].position.z==0.,"D13 read-only collar changed");
    std::cout<<"D13_SMOOTH_CUDA_OK center_z="<<z<<'\n';
  }
  return 0;
}
