// SPDX-License-Identifier: MIT
#include "guidedquad/guide_graph.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace guidedquad {
namespace {
using Id = std::uint32_t;
using EdgeKey = std::pair<Id, Id>;

EdgeKey ordered(Id a, Id b) { return {std::min(a, b), std::max(a, b)}; }

void validate(const TriangleMesh &mesh) {
  if (mesh.vertices.size() > std::numeric_limits<Id>::max()) {
    throw std::invalid_argument("vertex count exceeds 32-bit indices");
  }
  for (const Vec3 &p : mesh.vertices) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      throw std::invalid_argument("mesh has non-finite coordinates");
    }
  }
  for (const auto &tri : mesh.triangles) {
    if (tri[0] >= mesh.vertices.size() || tri[1] >= mesh.vertices.size() ||
        tri[2] >= mesh.vertices.size() || tri[0] == tri[1] ||
        tri[1] == tri[2] || tri[2] == tri[0]) {
      throw std::invalid_argument("mesh has invalid or degenerate triangle indices");
    }
  }
}

struct GraphAssembler {
  GuideGraph result;
  std::map<EdgeKey, Id> node_ids;
  std::map<EdgeKey, bool> unique_graph_edges;

  Id node(Id a, Id b, double t, const TriangleMesh &mesh) {
    if (t <= 1e-10) { b = a; t = 0.0; }
    else if (t >= 1.0 - 1e-10) { a = b; t = 0.0; }
    if (a > b) { std::swap(a, b); t = 1.0 - t; }
    const EdgeKey key{a, b};
    auto it = node_ids.find(key);
    if (it != node_ids.end()) { return it->second; }
    Vec3 p{mesh.vertices[a].x * (1.0 - t) + mesh.vertices[b].x * t,
           mesh.vertices[a].y * (1.0 - t) + mesh.vertices[b].y * t,
           mesh.vertices[a].z * (1.0 - t) + mesh.vertices[b].z * t};
    const Id id = static_cast<Id>(result.nodes.size());
    result.nodes.push_back({p, a, b, t});
    node_ids.emplace(key, id);
    return id;
  }

  void connect(Id a, Id b) {
    if (a == b) { return; }
    const EdgeKey key = ordered(a, b);
    if (unique_graph_edges.emplace(key, true).second) {
      result.edges.push_back({key.first, key.second});
    }
  }
};
} // namespace

void build_paths(GuideGraph &graph) {
  graph.paths.clear();
  std::vector<std::vector<Id>> incident(graph.nodes.size());
  for (Id e = 0; e < graph.edges.size(); ++e) {
    const GuideEdge &edge = graph.edges[e];
    if (edge.a >= graph.nodes.size() || edge.b >= graph.nodes.size()) {
      throw std::invalid_argument("guide edge references nonexistent node");
    }
    incident[edge.a].push_back(e);
    incident[edge.b].push_back(e);
  }
  for (auto &list : incident) std::sort(list.begin(), list.end());
  std::vector<bool> visited(graph.edges.size(), false);
  const auto walk = [&](Id first_node, Id first_edge) {
    GuidePath path;
    Id current = first_node, e = first_edge;
    path.nodes.push_back(current);
    for (;;) {
      if (visited[e]) break;
      visited[e] = true;
      const GuideEdge &edge = graph.edges[e];
      current = (edge.a == current) ? edge.b : edge.a;
      path.nodes.push_back(current);
      if (current == first_node) {
        path.closed = true;
        break;
      }
      if (incident[current].size() != 2) break;
      const Id next = incident[current][0] == e ? incident[current][1] : incident[current][0];
      if (visited[next]) break;
      e = next;
    }
    graph.paths.push_back(std::move(path));
  };
  for (Id n = 0; n < incident.size(); ++n) {
    if (incident[n].size() == 2) continue;
    for (const Id e : incident[n]) if (!visited[e]) walk(n, e);
  }
  for (Id e = 0; e < graph.edges.size(); ++e) {
    if (!visited[e]) walk(graph.edges[e].a, e);
  }
  for (const auto &list : incident) {
    if (list.size() > 2) {
      graph.warnings.emplace_back("guide junction detected (degree > 2): manual or solver resolution required");
      break;
    }
  }
}

GuideGraph extract_region_boundaries(const TriangleMesh &mesh,
                                     const std::vector<int> &regions,
                                     bool include_mesh_border) {
  validate(mesh);
  if (regions.size() != mesh.triangles.size()) {
    throw std::invalid_argument("one region ID is required per triangle");
  }
  std::map<EdgeKey, std::vector<Id>> incidents;
  for (Id f = 0; f < mesh.triangles.size(); ++f) {
    const auto &tri = mesh.triangles[f];
    for (int i = 0; i < 3; ++i) {
      incidents[ordered(tri[i], tri[(i + 1) % 3])].push_back(f);
    }
  }
  GraphAssembler out;
  for (const auto &entry : incidents) {
    const auto &faces = entry.second;
    if (faces.size() > 2) {
      out.result.warnings.emplace_back("non-manifold input edge skipped");
      continue;
    }
    const bool boundary = (faces.size() == 2 && regions[faces[0]] != regions[faces[1]]) ||
                          (include_mesh_border && faces.size() == 1);
    if (boundary) {
      const Id a = out.node(entry.first.first, entry.first.first, 0.0, mesh);
      const Id b = out.node(entry.first.second, entry.first.second, 0.0, mesh);
      out.connect(a, b);
    }
  }
  build_paths(out.result);
  return out.result;
}

GuideGraph extract_scalar_contours(const TriangleMesh &mesh,
                                   const std::vector<double> &values,
                                   double threshold) {
  validate(mesh);
  if (values.size() != mesh.vertices.size() || !std::isfinite(threshold)) {
    throw std::invalid_argument("invalid scalar array or contour threshold");
  }
  for (double value : values) if (!std::isfinite(value)) {
    throw std::invalid_argument("non-finite scalar value");
  }
  std::vector<bool> high(values.size());
  for (Id v = 0; v < values.size(); ++v) {
    high[v] = (values[v] > threshold) || (values[v] == threshold && (v % 2 == 0));
  }
  GraphAssembler out;
  for (const auto &tri : mesh.triangles) {
    Id crossings[3];
    int count = 0;
    for (int i = 0; i < 3; ++i) {
      const Id a = tri[i], b = tri[(i + 1) % 3];
      if (high[a] == high[b]) continue;
      double t = 0.5;
      const double diff = values[b] - values[a];
      if (diff != 0.0) t = (threshold - values[a]) / diff;
      crossings[count++] = out.node(a, b, std::clamp(t, 0.0, 1.0), mesh);
    }
    if (count == 2) out.connect(crossings[0], crossings[1]);
  }
  build_paths(out.result);
  return out.result;
}

} // namespace guidedquad
