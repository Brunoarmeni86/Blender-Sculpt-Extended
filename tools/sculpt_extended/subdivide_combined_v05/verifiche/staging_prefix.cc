#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
template<class T> struct Span {
  const T *p = nullptr; int n = 0;
  Span() = default; Span(const std::vector<T> &v) : p(v.data()), n(int(v.size())) {}
  int size() const { return n; }
  T operator[](int i) const { return p[i]; }
};
struct MDisps { float (*disps)[3]=nullptr; bool *hidden=nullptr; int totdisp=0; int level=0; };
struct MultiresSculptLayer { MDisps *disps=nullptr; int grids_num=0; int level=0; };
namespace bke::subdiv { int grid_size_from_level(int level) { return (1 << (level-1)) + 1; } }
#define MEM_SAFE_DELETE(p) do { delete[] (p); (p)=nullptr; } while(0)
#define BLI_BITMAP_NEW(n,label) new bool[n]()
#define BLI_BITMAP_SET(p,i,v) ((p)[i]=(v))
#define BLI_BITMAP_TEST_BOOL(p,i) ((p)[i])
struct MultiresSubdivisionPhaseTimer { template<class...T> MultiresSubdivisionPhaseTimer(T...) {} };
bool multires_subdivide_flag_v01(const char*) { return true; }
const char *BLI_getenv(const char*) { return nullptr; }
int multires_subdivide_lanes_v01(int,int) { return 4; }
template<class F> void multires_subdivide_grids_v01(int count,int,F &&fn) {
  std::vector<std::thread> workers;
  for(int t=0;t<4;++t) workers.emplace_back([&,t] { for(int i=t;i<count;i+=4) fn(i); });
  for(auto &worker:workers) worker.join();
}
