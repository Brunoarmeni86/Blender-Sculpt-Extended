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

static bool multires_stage_subdivided_layer_serial_v01(const MultiresSculptLayer &layer,
                                           const MDisps *base,
                                           MDisps *result,
                                           const int grids_num,
                                           const int new_level,
                                           const bool allow_sparse = false,
                                           const Span<int8_t> regional_support = {})
{
  if (!base || !result || !layer.disps || layer.grids_num != grids_num ||
      layer.level < 1 || new_level <= layer.level || new_level > 13)
  {
    return false;
  }
  const int old_side = bke::subdiv::grid_size_from_level(layer.level);
  const int new_side = bke::subdiv::grid_size_from_level(new_level);
  for (int corner = 0; corner < grids_num; corner++) {
    const MDisps &old = layer.disps[corner];
    MDisps &grid = result[corner];
    if ((!old.disps && !allow_sparse) || old.level != layer.level || old.totdisp != old_side * old_side ||
        !base[corner].disps || base[corner].level != new_level ||
        base[corner].totdisp != new_side * new_side || !grid.disps ||
        grid.level != new_level || grid.totdisp != new_side * new_side)
    {
      return false;
    }
    /* Retain resample_all's validation of the old layer, including hidden layers. */
    for (int i = 0; old.disps && i < old.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        if (!std::isfinite(old.disps[i][axis])) {
          return false;
        }
      }
    }
    const bool inactive = regional_support.size() == grids_num && !regional_support[corner];
    for (int i = 0; i < grid.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        const float delta = inactive ?
            base[corner].disps[i][axis] - base[corner].disps[i][axis] :
            grid.disps[i][axis] - base[corner].disps[i][axis];
        if (!std::isfinite(delta)) {
          return false;
        }
        if (!inactive) {
          grid.disps[i][axis] = delta;
        }
      }
    }
    if (inactive) {
      MEM_SAFE_DELETE(grid.disps);
    }
    /* The scratch surface carries the BASE hidden bitmap. Replace it with the
     * layer's own bitmap, using exactly the old resample_all mapping. */
    MEM_SAFE_DELETE(grid.hidden);
    if (old.hidden) {
      grid.hidden = BLI_BITMAP_NEW(grid.totdisp, "subdivided sculpt layer hidden");
      for (int y = 0; y < new_side; y++) {
        const float v = float(y) * float(old_side - 1) / float(new_side - 1);
        const int sy = int(std::round(v));
        for (int x = 0; x < new_side; x++) {
          const float u = float(x) * float(old_side - 1) / float(new_side - 1);
          const int sx = int(std::round(u));
          BLI_BITMAP_SET(grid.hidden, y * new_side + x,
                         BLI_BITMAP_TEST_BOOL(old.hidden, sy * old_side + sx));
        }
      }
    }
  }
  return true;
}

static bool multires_stage_subdivided_layer(const MultiresSculptLayer &layer,
                                           const MDisps *base,
                                           MDisps *result,
                                           const int grids_num,
                                           const int new_level,
                                           const bool allow_sparse = false,
                                           const Span<int8_t> regional_support = {})
{
  if (!multires_subdivide_flag_v01("BLENDER_SCULPT_SUBDIVIDE_PARALLEL_V01")) {
    return multires_stage_subdivided_layer_serial_v01(
        layer, base, result, grids_num, new_level, allow_sparse, regional_support);
  }

  if (!base || !result || !layer.disps || layer.grids_num != grids_num ||
      layer.level < 1 || new_level <= layer.level || new_level > 13)
  {
    return false;
  }
  const int old_side = bke::subdiv::grid_size_from_level(layer.level);
  const int new_side = bke::subdiv::grid_size_from_level(new_level);
  const auto validate = [&](const int corner) {
    const MDisps &old = layer.disps[corner];
    const MDisps &grid = result[corner];
    if ((!old.disps && !allow_sparse) || old.level != layer.level || old.totdisp != old_side * old_side ||
        !base[corner].disps || base[corner].level != new_level ||
        base[corner].totdisp != new_side * new_side || !grid.disps ||
        grid.level != new_level || grid.totdisp != new_side * new_side)
    {
      return false;
    }
    /* Retain resample_all's validation of the old layer, including hidden layers. */
    for (int i = 0; old.disps && i < old.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        if (!std::isfinite(old.disps[i][axis])) {
          return false;
        }
      }
    }
    return true;
  };
  std::atomic<bool> valid{true};
  {
    MultiresSubdivisionPhaseTimer timer("validate_delta_v01", "layer", new_level, grids_num);
    multires_subdivide_grids_v01(grids_num, new_side * new_side, [&](const int corner) {
      if (!validate(corner)) {
        valid.store(false, std::memory_order_relaxed);
      }
    });
  }
  if (!valid.load(std::memory_order_relaxed)) {
    return false;
  }
  multires_subdivide_grids_v01(grids_num, new_side * new_side, [&](const int corner) {
    const MDisps &old = layer.disps[corner];
    MDisps &grid = result[corner];
    const bool inactive = regional_support.size() == grids_num && !regional_support[corner];
    for (int i = 0; i < grid.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        const float delta = inactive ?
            base[corner].disps[i][axis] - base[corner].disps[i][axis] :
            grid.disps[i][axis] - base[corner].disps[i][axis];
        if (!std::isfinite(delta)) {
          valid.store(false, std::memory_order_relaxed);
        }
        if (!inactive) {
          grid.disps[i][axis] = delta;
        }
      }
    }
    if (inactive) {
      MEM_SAFE_DELETE(grid.disps);
    }
    /* The scratch surface carries the BASE hidden bitmap. Replace it with the
     * layer's own bitmap, using exactly the old resample_all mapping. */
    MEM_SAFE_DELETE(grid.hidden);
    if (old.hidden) {
      grid.hidden = BLI_BITMAP_NEW(grid.totdisp, "subdivided sculpt layer hidden");
      for (int y = 0; y < new_side; y++) {
        const float v = float(y) * float(old_side - 1) / float(new_side - 1);
        const int sy = int(std::round(v));
        for (int x = 0; x < new_side; x++) {
          const float u = float(x) * float(old_side - 1) / float(new_side - 1);
          const int sx = int(std::round(u));
          BLI_BITMAP_SET(grid.hidden, y * new_side + x,
                         BLI_BITMAP_TEST_BOOL(old.hidden, sy * old_side + sx));
        }
      }
    }
  });
  if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
    std::printf("[SUBDIVIDE_WORK_V01] grids=%d area=%d lanes=%d valid=%d\n",
                grids_num, new_side * new_side,
                multires_subdivide_lanes_v01(grids_num, new_side * new_side),
                int(valid.load(std::memory_order_relaxed)));
  }
  return valid.load(std::memory_order_relaxed);
}


void release(std::vector<MDisps> &a) { for(auto &g:a) { delete[] g.disps; delete[] g.hidden; } }
using Stage = bool (*)(const MultiresSculptLayer&, const MDisps*, MDisps*, int, int, bool, Span<int8_t>);
void trial(Stage stage, bool sparse, bool invalid_base, bool invalid_old) {
  const int count=8, old_side=3, side=5;
  std::vector<MDisps> base(count), old(count), expected(count), actual(count);
  std::vector<int8_t> support(count,1); support[1]=support[4]=support[6]=0;
  for(int g=0;g<count;++g) {
    old[g].level=2; old[g].totdisp=old_side*old_side;
    if(!sparse || support[g]) old[g].disps=new float[old_side*old_side][3]();
    if(g==4 || g==3) { old[g].hidden=new bool[old_side*old_side](); old[g].hidden[4]=true; }
    for(auto *v:{&base,&expected,&actual}) {
      (*v)[g].level=3; (*v)[g].totdisp=side*side;
      (*v)[g].disps=new float[side*side][3]();
      (*v)[g].hidden=new bool[side*side](); (*v)[g].hidden[0]=true;
    }
    for(int i=0;i<side*side;++i) for(int a=0;a<3;++a) {
      base[g].disps[i][a]=float((g+1)*17+i*3+a)/31.0f;
      expected[g].disps[i][a]=base[g].disps[i][a]+(support[g] ? .125f : 0.0f);
      actual[g].disps[i][a]=support[g] ? expected[g].disps[i][a] : 12345.0f;
    }
  }
  if(invalid_base) base[1].disps[3][1]=std::numeric_limits<float>::infinity();
  if(invalid_old) {
    if(!old[1].disps) old[1].disps=new float[old_side*old_side][3]();
    old[1].disps[3][1]=std::numeric_limits<float>::quiet_NaN();
  }
  MultiresSculptLayer layer{old.data(),count,2};
  const bool wanted=stage(layer,base.data(),expected.data(),count,3,true,{});
  const bool got=stage(layer,base.data(),actual.data(),count,3,true,support);
  assert(wanted==got);
  if(got) for(int g=0;g<count;++g) {
    assert(actual[g].level==expected[g].level && actual[g].totdisp==expected[g].totdisp);
    assert(bool(actual[g].hidden)==bool(expected[g].hidden));
    if(actual[g].hidden) for(int i=0;i<side*side;++i) assert(actual[g].hidden[i]==expected[g].hidden[i]);
    if(support[g]) assert(std::memcmp(actual[g].disps,expected[g].disps,side*side*sizeof(float[3]))==0);
    else {
      assert(!actual[g].disps);
      for(int i=0;i<side*side;++i) for(int a=0;a<3;++a) assert(expected[g].disps[i][a]==0.0f);
    }
  }
  release(base); release(old); release(expected); release(actual);
}
int main() {
  for(Stage stage:{multires_stage_subdivided_layer_serial_v01,multires_stage_subdivided_layer})
    for(bool sparse:{false,true}) {
      trial(stage,sparse,false,false); trial(stage,sparse,true,false); trial(stage,sparse,false,true);
    }
  std::cout << "PASS actual V05 serial/parallel staging: V04-equivalent deltas, implicit zeros, sparse inputs, hidden mapping, nonfinite base and old-layer rejection\n";
}
