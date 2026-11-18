// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace guidedquad {
struct Vec3 { double x = 0.0, y = 0.0, z = 0.0; };
struct TriangleMesh {
  std::vector<Vec3> vertices;
  std::vector<std::array<std::uint32_t, 3>> triangles;
};
struct GuideNode {
  Vec3 position;
  std::uint32_t a = 0, b = 0;
  double t = 0.0;
};
struct GuideEdge { std::uint32_t a = 0, b = 0; };
struct GuidePath { std::vector<std::uint32_t> nodes; bool closed = false; };
struct GuideGraph {
  std::vector<GuideNode> nodes;
  std::vector<GuideEdge> edges;
  std::vector<GuidePath> paths;
  std::vector<std::string> warnings;
};
GuideGraph extract_region_boundaries(const TriangleMesh &mesh,
                                     const std::vector<int> &triangle_regions,
                                     bool include_mesh_border = false);
GuideGraph extract_scalar_contours(const TriangleMesh &mesh,
                                   const std::vector<double> &values,
                                   double threshold);
void build_paths(GuideGraph &graph);
} // namespace guidedquad
