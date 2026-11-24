/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "symmetry_dab_targets.hh"
#include <cassert>
#include <iostream>

int main()
{
  using namespace sculpt_physics;
  std::vector<MirrorDabSample> samples{{7,0,{1,2,0}},{7,1,{-1,2,0}},
                                      {7,0,{1,2,0}},{8,0,{2,0,0}},
                                      {9,1,{-2,0,0}}};
  const auto a=merge_mirror_dab_targets(samples,1);
  std::reverse(samples.begin(),samples.end());
  const auto b=merge_mirror_dab_targets(samples,1);
  assert(a.size()==3 && b.size()==3);
  for (std::size_t i=0;i<a.size();++i) {
    assert(a[i].vertex==b[i].vertex && a[i].translation==b[i].translation);
  }
  assert((a[0].translation==std::array<double,3>{0,4,0}));
  assert((a[1].translation==std::array<double,3>{2,0,0}));
  assert((a[2].translation==std::array<double,3>{-2,0,0}));
  bool rejected=false;
  try { (void)merge_mirror_dab_targets({{0,2,{1,0,0}}},1); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  rejected=false;
  try { (void)merge_mirror_dab_targets({{0,0,{1,0,0}},{0,0,{2,0,0}}},1); }
  catch (const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  std::cout << "Mirror merge: order, duplicate, overlap and pass checks passed\n";
}
