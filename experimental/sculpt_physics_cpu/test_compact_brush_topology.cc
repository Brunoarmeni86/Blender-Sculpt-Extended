/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "compact_brush_topology.hh"

#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

using sculpt_physics::CompactBrushTopology;

int main()
{
  using Edge = std::pair<std::size_t, std::size_t>;
  std::vector<Edge> edges{{0,1},{1,2},{2,3},{3,4},{0,0},{1,2}};
  auto read = [&](std::size_t i) { return edges.at(i); };
  CompactBrushTopology graph(6, edges.size(), read);
  auto a = graph.gather({2,1,2}, read);
  assert((a.vertices == std::vector<std::size_t>{0,1,2,3}));
  assert((a.edges == std::vector<std::uint32_t>{0,1,2,5}));
  auto b = graph.gather({5}, read);
  assert((b.vertices == std::vector<std::size_t>{5}) && b.edges.empty());
  assert(graph.gather({}, read).vertices.empty());
  bool rejected = false;
  try { (void)graph.gather({6}, read); }
  catch (const std::out_of_range &) { rejected = true; }
  assert(rejected);
  edges[1] = {3,4};
  rejected = false;
  try { (void)graph.gather({1}, read); }
  catch (const std::logic_error &) { rejected = true; }
  assert(rejected);

  constexpr std::size_t n = 1000000;
  auto chain = [](std::size_t i) { return Edge{i,i+1}; };
  CompactBrushTopology large(n, n-1, chain);
  std::size_t reads = 0;
  auto counted = [&](std::size_t i) { ++reads; return chain(i); };
  auto local = large.gather({500000,500001}, counted);
  assert(local.vertices.size()==4 && local.edges.size()==3 && reads==4);
  assert(large.storage_bytes() <= 16*n);
  std::cout << "Compact index: " << large.storage_bytes()
            << " bytes; queried edges: " << reads << '\n';
  return 0;
}
