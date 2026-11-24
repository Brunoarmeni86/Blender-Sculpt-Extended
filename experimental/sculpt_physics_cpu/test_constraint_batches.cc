/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "constraint_batches.hh"
#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

int main()
{
  using Edge=std::pair<std::size_t,std::size_t>;
  using sculpt_physics::build_constraint_batches;
  const std::vector<Edge> edges{{0,1},{1,2},{2,3},{3,0},{0,2},{4,5},{0,1}};
  auto read=[&](std::size_t i){return edges.at(i);};
  const auto batch=build_constraint_batches(6,edges.size(),read);
  const auto again=build_constraint_batches(6,edges.size(),read);
  assert(batch.offsets==again.offsets && batch.bond_ids==again.bond_ids);
  assert(batch.offsets.front()==0 && batch.offsets.back()==edges.size());
  std::vector<int> seen(edges.size());
  for (std::size_t c=1;c<batch.offsets.size();++c) {
    std::vector<bool> touched(6,false);
    for (std::size_t j=batch.offsets[c-1];j<batch.offsets[c];++j) {
      const auto id=batch.bond_ids[j];
      const auto [a,b]=edges.at(id);
      assert(!touched[a] && !touched[b]);
      touched[a]=touched[b]=true;
      ++seen[id];
    }
  }
  for (const int n : seen) assert(n==1);
  const auto empty=build_constraint_batches(0,0,read);
  assert(empty.offsets.size()==1 && empty.bond_ids.empty());
  bool rejected=false;
  try { (void)build_constraint_batches(1,1,[](std::size_t){return Edge{0,0};}); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  constexpr std::size_t n=1000000;
  const auto chain=build_constraint_batches(n,n-1,[](std::size_t i){return Edge{i,i+1};});
  assert(chain.offsets.size()==3 && chain.bond_ids.size()==n-1);
  std::cout << "1M vertices: " << chain.offsets.size()-1 << " disjoint batches\n";
}
