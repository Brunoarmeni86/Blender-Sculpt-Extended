/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../source/blender/blenkernel/intern/pbvh_face_indices.hh"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

using blender::bke::pbvh::sort_unique_face_indices;
static std::vector<int> dense_reference(int faces, const std::vector<int> &ids)
{
  std::vector<unsigned char> flags(faces, 0);
  for (int id : ids) { flags[id] = 1; }
  std::vector<int> result;
  for (int i=0; i<faces; i++) { if(flags[i]) result.push_back(i); }
  return result;
}
int main()
{
  std::mt19937 rng(20261023);
  int cases=0;
  for(int faces : {1, 7, 8, 64, 1000, 100000}) {
    for(int count : {0, 1, 2, 7, 128, 1024}) {
      for(int repeat=0;repeat<8;repeat++) {
        std::vector<int> selected;
        for(int i=0;i<count;i++) selected.push_back(rng()%faces);
        auto expected=dense_reference(faces,selected);
        sort_unique_face_indices(selected);
        if(selected!=expected) std::abort();
        cases++;
      }
    }
  }
  volatile long long checksum=0;
  std::vector<int> ids;
  for(int i=0;i<128;i++) ids.push_back(rng()%1000000);
  auto start=std::chrono::steady_clock::now();
  for(int i=0;i<200;i++) { auto r=dense_reference(1000000,ids); checksum+=r.size(); }
  auto middle=std::chrono::steady_clock::now();
  for(int i=0;i<200;i++) { auto r=ids; sort_unique_face_indices(r); checksum+=r.size(); }
  auto end=std::chrono::steady_clock::now();
  std::printf("PASS %d exact selections; isolated selection benchmark: dense %.3fms sparse %.3fms (200 iterations, 1M faces / 128 IDs), checksum %lld\n", cases,
      std::chrono::duration<double,std::milli>(middle-start).count(),
      std::chrono::duration<double,std::milli>(end-middle).count(), checksum);
}
