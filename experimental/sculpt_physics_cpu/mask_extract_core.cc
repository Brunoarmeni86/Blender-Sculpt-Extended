/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mask_extract_core.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace sculpt_physics {
namespace {
struct Edge {
  std::size_t lo, hi;
  bool operator==(const Edge &rhs) const { return lo == rhs.lo && hi == rhs.hi; }
};
struct EdgeHash {
  std::size_t operator()(const Edge &e) const
  {
    return std::hash<std::size_t>{}(e.lo) ^
           (std::hash<std::size_t>{}(e.hi) + 0x9e3779b9 +
            (std::hash<std::size_t>{}(e.lo) << 6));
  }
};
struct Corner {
  std::size_t src_a, src_b; // src_a==src_b means an original vertex.
  Vec3 position;
  UV2 uv;
  RGBA color;
  double mask;
};
UV2 lerp(UV2 a, UV2 b, double t)
{
  return {a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
}
RGBA lerp(RGBA a, RGBA b, double t)
{
  return {a.r + (b.r - a.r)*t, a.g + (b.g - a.g)*t,
          a.b + (b.b - a.b)*t, a.a + (b.a - a.a)*t};
}
Corner crossing(const Corner &a, const Corner &b, double threshold)
{
  const double denom = b.mask - a.mask;
  if (std::abs(denom) < 1e-15) throw std::logic_error("No threshold crossing");
  const double t = std::clamp((threshold - a.mask) / denom, 0.0, 1.0);
  if (t <= 1e-12) return a;
  if (t >= 1.0 - 1e-12) return b;
  // In input each crossing endpoint is an original vertex.
  return {a.src_a, b.src_a, a.position + (b.position - a.position) * t,
          lerp(a.uv, b.uv, t), lerp(a.color, b.color, t), threshold};
}
}  // namespace

TriangleMesh extract_mask_high_fidelity(const TriangleMesh &source, double threshold)
{
  if (!std::isfinite(threshold) || threshold < 0.0 || threshold > 1.0 ||
      source.mask.size() != source.positions.size()) {
    throw std::invalid_argument("Invalid mask or threshold");
  }
  for (double m : source.mask) {
    if (!std::isfinite(m) || m < 0.0 || m > 1.0) {
      throw std::invalid_argument("Mask must be finite and within [0, 1]");
    }
  }
  // UVs may intentionally span UDIMs and colors can be HDR, but all
  // interpolated corner attributes and input positions must be finite.
  for (const Vec3 &p : source.positions) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      throw std::invalid_argument("Non-finite source position");
    }
  }
  for (const SurfaceTriangle &face : source.faces) {
    for (std::size_t k = 0; k < 3; ++k) {
      if (face.vertices[k] >= source.positions.size()) {
        throw std::out_of_range("Face vertex out of range");
      }
      const UV2 &uv = face.uv[k];
      const RGBA &c = face.colors[k];
      if (!std::isfinite(uv.u) || !std::isfinite(uv.v) ||
          !std::isfinite(c.r) || !std::isfinite(c.g) ||
          !std::isfinite(c.b) || !std::isfinite(c.a)) {
        throw std::invalid_argument("Non-finite corner UV/color attribute");
      }
    }
  }
  TriangleMesh out;
  std::unordered_map<std::size_t, std::size_t> original_vertices;
  std::unordered_map<Edge, std::size_t, EdgeHash> edge_vertices;
  auto get_vertex = [&](const Corner &v) -> std::size_t {
    if (v.src_a == v.src_b) {
      const auto it = original_vertices.find(v.src_a);
      if (it != original_vertices.end()) return it->second;
      const std::size_t index = out.positions.size();
      original_vertices[v.src_a] = index;
      out.positions.push_back(v.position);
      out.mask.push_back(v.mask);
      return index;
    }
    const Edge e{std::min(v.src_a, v.src_b), std::max(v.src_a, v.src_b)};
    const auto it = edge_vertices.find(e);
    if (it != edge_vertices.end()) return it->second;
    const std::size_t index = out.positions.size();
    edge_vertices[e] = index;
    out.positions.push_back(v.position);
    out.mask.push_back(v.mask);
    return index;
  };

  for (const SurfaceTriangle &face : source.faces) {
    std::array<Corner, 3> original;
    for (std::size_t k = 0; k < 3; ++k) {
      const auto vi = face.vertices[k];
      if (vi >= source.positions.size()) throw std::out_of_range("Face vertex out of range");
      original[k] = {vi, vi, source.positions[vi], face.uv[k], face.colors[k], source.mask[vi]};
    }
    std::vector<Corner> clipped;
    for (std::size_t k = 0; k < 3; ++k) {
      const Corner &a = original[(k + 2) % 3];
      const Corner &b = original[k];
      const bool a_inside = a.mask >= threshold;
      const bool b_inside = b.mask >= threshold;
      if (a_inside != b_inside) clipped.push_back(crossing(a, b, threshold));
      if (b_inside) clipped.push_back(b);
    }
    // Remove duplicates at vertices whose masks exactly equal the threshold.
    std::vector<Corner> clean;
    for (const Corner &v : clipped) {
      const auto same_key = [&](const Corner &other) {
        return std::min(v.src_a, v.src_b) == std::min(other.src_a, other.src_b) &&
               std::max(v.src_a, v.src_b) == std::max(other.src_a, other.src_b);
      };
      if (clean.empty() || !same_key(clean.back())) clean.push_back(v);
    }
    if (clean.size() > 1) {
      const Corner &a = clean.front();
      const Corner &b = clean.back();
      if (std::min(a.src_a,a.src_b) == std::min(b.src_a,b.src_b) &&
          std::max(a.src_a,a.src_b) == std::max(b.src_a,b.src_b)) clean.pop_back();
    }
    if (clean.size() < 3) continue;
    for (std::size_t k = 1; k + 1 < clean.size(); ++k) {
      SurfaceTriangle tri;
      const std::array<Corner, 3> corners{clean[0], clean[k], clean[k+1]};
      for (std::size_t j = 0; j < 3; ++j) {
        tri.vertices[j] = get_vertex(corners[j]);
        tri.uv[j] = corners[j].uv;
        tri.colors[j] = corners[j].color;
      }
      tri.material_id = face.material_id;
      out.faces.push_back(tri);
    }
  }
  return out;
}
}  // namespace sculpt_physics
