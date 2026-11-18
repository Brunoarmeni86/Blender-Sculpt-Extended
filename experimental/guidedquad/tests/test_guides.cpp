// SPDX-License-Identifier: MIT
#include "guidedquad/guide_graph.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace guidedquad;

static TriangleMesh square() {
  return {{{0,0,0},{1,0,0},{1,1,0},{0,1,0}}, {{0,1,2},{0,2,3}}};
}

int main() {
  const auto mesh = square();
  {
    const auto g = extract_region_boundaries(mesh, {1,2});
    assert(g.edges.size() == 1 && g.nodes.size() == 2);
    assert(g.paths.size() == 1 && !g.paths[0].closed);
    for (const auto &node : g.nodes) assert(node.a == node.b);
  }
  {
    const auto g = extract_region_boundaries(mesh, {1,1});
    assert(g.edges.empty() && g.paths.empty());
  }
  {
    const auto g = extract_scalar_contours(mesh, {-1,1,1,-1}, 0);
    assert(g.nodes.size() == 3 && g.edges.size() == 2);
    assert(g.paths.size() == 1 && g.paths[0].nodes.size() == 3);
    for (const auto &node : g.nodes) assert(std::fabs(node.position.x - 0.5) < 1e-12);
  }
  {
    const TriangleMesh fan = {{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0},{0,0,0}},
                              {{0,1,4},{1,2,4},{2,3,4},{3,0,4}}};
    const auto g = extract_scalar_contours(fan, {-1,-1,-1,-1,1}, 0);
    assert(g.nodes.size() == 4 && g.edges.size() == 4);
    assert(g.paths.size() == 1 && g.paths[0].closed);
  }
  {
    const auto g = extract_scalar_contours(mesh, {0,1,1,-1}, 0);
    assert(g.edges.size() >= 1);
  }
  {
    bool caught = false;
    try { extract_region_boundaries(mesh, {1}); }
    catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
  }
  {
    const TriangleMesh nonmanifold = {{{0,0,0},{1,0,0},{0,1,0},{0,-1,0},{0,0,1}},
                                      {{0,1,2},{1,0,3},{0,1,4}}};
    const auto g = extract_region_boundaries(nonmanifold, {0,1,2});
    assert(!g.warnings.empty());
  }
  {
    auto invalid = mesh;
    invalid.triangles[0][0] = 100;
    bool caught = false;
    try { extract_region_boundaries(invalid, {1,2}); }
    catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
  }
  std::cout << "Guide graph tests passed.\n";
}
