/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <array>
#include <cstddef>
#include <vector>

namespace sculpt_physics {

struct RGBA {
  double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
};

struct SurfaceTriangle {
  std::array<std::size_t, 3> vertices{};
  std::array<UV2, 3> uv{};
  std::array<RGBA, 3> colors{};
  int material_id = 0;
};

struct TriangleMesh {
  std::vector<Vec3> positions;
  std::vector<double> mask; // Vertex-domain mask, 0..1.
  std::vector<SurfaceTriangle> faces; // UV/color are on the corner domain.
};

/* Extract triangle mesh by clipping each face against the HR mask threshold.
 * Returns independent topology and keeps per-corner UV/color seams and material IDs.
 * Boundary cut vertices are welded by source edge ID; source is not modified.
 * Does not create a new Blender Multires hierarchy. */
TriangleMesh extract_mask_high_fidelity(const TriangleMesh &source, double threshold);

}  // namespace sculpt_physics
