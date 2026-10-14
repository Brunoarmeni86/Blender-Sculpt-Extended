#ifdef NDEBUG
#error Assertions required
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
// Actual BKE Polypaint functions and Color Undo capture/restore; native scene/allocator stubbed.
#include <algorithm>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_set>
#include "BKE_multires_polypaint_prepared.hh"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include "polypaint_cpu_math.hh"
std::atomic<int> live_arrays{0};
template<class T>T* MEM_new_array_zeroed(int n,const char*){++live_arrays;return new T[n]{};}
template<class T>T* MEM_new_array_uninitialized(int n,const char*){++live_arrays;return new T[n];}
template<class T>void MEM_delete(T*p){if(p)--live_arrays;delete[] p;}
#define MEM_SAFE_DELETE(p) do {MEM_delete(p);p=nullptr;}while(0)
struct IndexRange {int start_=0,n=0;explicit IndexRange(int n):n(n){};IndexRange(int s,int n):start_(s),n(n){}
 int start()const{return start_;}int size()const{return n;}int one_after_last()const{return start_+n;}
 struct It{int i;int operator*()const{return i;}It&operator++(){++i;return *this;}bool operator!=(It o)const{return i!=o.i;}};
 It begin()const{return{start_};}It end()const{return{start_+n};}};
template<class T>struct Span{const std::vector<T>*p;int size()const{return p?p->size():0;}
 const T&operator[](int i)const{return p->at(i);}auto begin()const{return p->begin();}auto end()const{return p->end();}IndexRange index_range()const{return IndexRange(size());}};
template<class T>struct OffsetIndices{const std::vector<int>*p;int size()const{return p->size()-1;}
 IndexRange operator[](int i)const{return{p->at(i),p->at(i+1)-p->at(i)};}};
struct ColorGeometry4f{float r,g,b,a;};
template<class T>struct VArray{const std::vector<T>*p=nullptr;operator bool()const{return p!=nullptr;}
 int size()const{return p?p->size():0;}const T&operator[](int i)const{return p->at(i);}};
enum class AttrDomain{Corner};
struct Mesh{const char*active_color_attribute="SkinColor";int corners_num=8;std::vector<int> offsets{0,4,8},corner_faces{0,0,0,0,1,1,1,1};
 std::vector<ColorGeometry4f>colors=std::vector<ColorGeometry4f>(8,{.2f,.3f,.4f,1});bool has_attribute=true;
 struct Attr{const Mesh*m;template<class T>std::optional<VArray<T>>lookup(const char*,AttrDomain)const{return VArray<T>{m->has_attribute?&m->colors:nullptr};}};
 Attr attributes()const{return{this};}Span<int>corner_to_face_map()const{return{&corner_faces};}
 OffsetIndices<int>faces()const{return{&offsets};}};
struct MultiresColorGrid{float(*colors)[4]=nullptr;int totcolor=0,level=0;};
struct MultiresModifierData{MultiresColorGrid*sculpt_color_grids=nullptr;int sculpt_color_grids_num=0,sculpt_color_level=0,sculptlvl=3,totlvl=3;};
namespace blender::bke{namespace subdiv{int grid_size_from_level(int l){assert(l>=1&&l<=13);return (1<<(l-1))+1;}}
namespace multires_layers{bool polypaint_resample(MultiresModifierData&,int);}}
using namespace blender;
using namespace blender::bke;
namespace blender::bke::multires_layers {
void polypaint_clear(MultiresModifierData &mmd)
{
  if (mmd.sculpt_color_grids) {
    for (int i = 0; i < mmd.sculpt_color_grids_num; i++) {
      MEM_SAFE_DELETE(mmd.sculpt_color_grids[i].colors);
    }
    MEM_delete(mmd.sculpt_color_grids);
  }
  mmd.sculpt_color_grids = nullptr;
  mmd.sculpt_color_grids_num = 0;
  mmd.sculpt_color_level = 0;
}
bool polypaint_ensure(MultiresModifierData &mmd, const int level, const int grids_num)
{
  if (level < 1 || level > 13 || grids_num < 0) {
    return false;
  }
  if (mmd.sculpt_color_grids) {
    /* A Sculpt Level change must NOT resample/destroy the high-res master.
     * Topology changes are handled by explicit remapping, never by clear(). */
    return mmd.sculpt_color_grids_num == grids_num &&
           mmd.sculpt_color_level >= 1 && mmd.sculpt_color_level <= 13;
  }
  if (grids_num > 0) {
    mmd.sculpt_color_grids = MEM_new_array_zeroed<MultiresColorGrid>(
        grids_num, "multires polypaint grids");
  }
  mmd.sculpt_color_grids_num = grids_num;
  mmd.sculpt_color_level = level;
  return true;
}
float (*polypaint_grid_ensure_master(MultiresModifierData &mmd, const int corner))[4]
{
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num ||
      mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13)
  {
    return nullptr;
  }
  MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  const int area = side * side;
  if (grid.colors && (grid.level != mmd.sculpt_color_level || grid.totcolor != area)) {
    /* Fail closed: never return malformed storage to brush/undo writers. */
    return nullptr;
  }
  if (!grid.colors) {
    grid.colors = MEM_new_array_uninitialized<float[4]>(area, "multires polypaint color");
    for (int i = 0; i < area; i++) {
      grid.colors[i][0] = grid.colors[i][1] = grid.colors[i][2] = grid.colors[i][3] = 1.0f;
    }
    grid.totcolor = area;
    grid.level = mmd.sculpt_color_level;
  }
  return grid.colors;
}
float (*polypaint_grid_ensure(MultiresModifierData &mmd, const int corner))[4]
{
  /* Legacy writers index using the current CCG side. Never allow those writes
   * at a different (lower or higher) level than the stored color master. */
  return mmd.sculptlvl == mmd.sculpt_color_level ? polypaint_grid_ensure_master(mmd, corner) :
                                                  nullptr;
}
const float (*polypaint_grid(const MultiresModifierData &mmd, const int corner))[4]
{
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num ||
      mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13)
  {
    return nullptr;
  }
  const MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  return grid.colors && grid.level == mmd.sculpt_color_level && grid.totcolor == side * side ?
             grid.colors :
             nullptr;
}
bool polypaint_sample_grid(const MultiresColorGrid &grid,
                           const float u,
                           const float v,
                           float r_rgba[4])
{
  if (r_rgba == nullptr) {
    return false;
  }
  r_rgba[0] = r_rgba[1] = r_rgba[2] = r_rgba[3] = 1.0f;
  if (!grid.colors || grid.level < 1 || grid.level > 13 || !std::isfinite(u) ||
      !std::isfinite(v))
  {
    return false;
  }
  const int side = subdiv::grid_size_from_level(grid.level);
  if (side < 2 || grid.totcolor != side * side) {
    return false;
  }
  const float x = std::clamp(u, 0.0f, 1.0f) * float(side - 1);
  const float y = std::clamp(v, 0.0f, 1.0f) * float(side - 1);
  const int x0 = int(x), y0 = int(y);
  const int x1 = std::min(x0 + 1, side - 1), y1 = std::min(y0 + 1, side - 1);
  const float tx = x - float(x0), ty = y - float(y0);
  for (int channel = 0; channel < 4; channel++) {
    const float a = grid.colors[y0 * side + x0][channel] * (1.0f - tx) +
                    grid.colors[y0 * side + x1][channel] * tx;
    const float b = grid.colors[y1 * side + x0][channel] * (1.0f - tx) +
                    grid.colors[y1 * side + x1][channel] * tx;
    r_rgba[channel] = a * (1.0f - ty) + b * ty;
  }
  return true;
}
bool polypaint_sample(const MultiresModifierData &mmd,
                      const int corner,
                      const float u,
                      const float v,
                      float r_rgba[4])
{
  if (r_rgba == nullptr) {
    return false;
  }
  r_rgba[0] = r_rgba[1] = r_rgba[2] = r_rgba[3] = 1.0f;
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num) {
    return false;
  }
  return polypaint_sample_grid(mmd.sculpt_color_grids[corner], u, v, r_rgba);
}
bool polypaint_grid_discard(MultiresModifierData &mmd, const int corner)
{
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num) {
    return false;
  }
  MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
  MEM_SAFE_DELETE(grid.colors);
  grid.totcolor = 0;
  grid.level = mmd.sculpt_color_level;
  return true;
}
bool polypaint_resample(MultiresModifierData &mmd, const int new_level)
{
  if (!mmd.sculpt_color_grids || new_level < 1 || new_level > 13 ||
      mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13)
  {
    return false;
  }
  if (new_level == mmd.sculpt_color_level) {
    return true;
  }
  const int old_side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  const int new_side = subdiv::grid_size_from_level(new_level);
  /* Preflight all grids before changing any: invalid metadata would otherwise
   * read past the source and partially corrupt the persistent color master. */
  for (int corner = 0; corner < mmd.sculpt_color_grids_num; corner++) {
    const MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
    if (grid.colors && (grid.level != mmd.sculpt_color_level ||
                        grid.totcolor != old_side * old_side))
    {
      return false;
    }
  }
  for (int corner = 0; corner < mmd.sculpt_color_grids_num; corner++) {
    MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
    if (!grid.colors) {
      continue;
    }
    float (*colors)[4] = MEM_new_array_uninitialized<float[4]>(new_side * new_side,
                                                               "resampled polypaint");
    for (int y = 0; y < new_side; y++) {
      const float v = float(y) * float(old_side - 1) / float(new_side - 1);
      const int y0 = int(v), y1 = std::min(y0 + 1, old_side - 1);
      const float fy = v - y0;
      for (int x = 0; x < new_side; x++) {
        const float u = float(x) * float(old_side - 1) / float(new_side - 1);
        const int x0 = int(u), x1 = std::min(x0 + 1, old_side - 1);
        const float fx = u - x0;
        for (int c = 0; c < 4; c++) {
          const float a = grid.colors[y0 * old_side + x0][c] * (1.0f - fx) +
                          grid.colors[y0 * old_side + x1][c] * fx;
          const float b = grid.colors[y1 * old_side + x0][c] * (1.0f - fx) +
                          grid.colors[y1 * old_side + x1][c] * fx;
          colors[y * new_side + x][c] = a * (1.0f - fy) + b * fy;
        }
      }
    }
    MEM_delete(grid.colors);
    grid.colors = colors;
    grid.totcolor = new_side * new_side;
    grid.level = new_level;
  }
  mmd.sculpt_color_level = new_level;
  return true;
}
static bool polypaint_base_patch(const Mesh &base_mesh,
                                 const char *attribute_name,
                                 const int corner,
                                 float r_patch[4][4])
{
  if (!attribute_name || attribute_name[0] == '\0' || corner < 0 ||
      corner >= base_mesh.corners_num)
  {
    return false;
  }
  const VArray<ColorGeometry4f> colors = *base_mesh.attributes().lookup<ColorGeometry4f>(
      attribute_name, AttrDomain::Corner);
  if (!colors || colors.size() != base_mesh.corners_num) {
    return false;
  }
  const Span<int> corner_faces = base_mesh.corner_to_face_map();
  if (corner >= corner_faces.size()) {
    return false;
  }
  const int face_index = corner_faces[corner];
  const OffsetIndices<int> faces = base_mesh.faces();
  if (face_index < 0 || face_index >= faces.size()) {
    return false;
  }
  const IndexRange face = faces[face_index];
  const int count = face.size();
  if (count < 3 || corner < face.start() || corner >= face.one_after_last()) {
    return false;
  }
  const int offset = corner - face.start();
  const int next = face.start() + (offset + 1) % count;
  const int prev = face.start() + (offset + count - 1) % count;
  const ColorGeometry4f a = colors[corner];
  const ColorGeometry4f b = colors[next];
  const ColorGeometry4f d = colors[prev];
  const float values_a[4] = {a.r, a.g, a.b, a.a};
  const float values_b[4] = {b.r, b.g, b.b, b.a};
  const float values_d[4] = {d.r, d.g, d.b, d.a};
  for (int c = 0; c < 4; c++) {
    r_patch[0][c] = 0.0f;
    r_patch[1][c] = (values_a[c] + values_b[c]) * 0.5f;
    r_patch[2][c] = (values_a[c] + values_d[c]) * 0.5f;
    r_patch[3][c] = values_a[c];
  }
  for (const int face_corner : face) {
    const ColorGeometry4f value = colors[face_corner];
    r_patch[0][0] += value.r / float(count);
    r_patch[0][1] += value.g / float(count);
    r_patch[0][2] += value.b / float(count);
    r_patch[0][3] += value.a / float(count);
  }
  return true;
}
bool polypaint_sample_with_base(const MultiresModifierData &mmd,
                                const Mesh &base_mesh,
                                const char *attribute_name,
                                const int corner,
                                const float u,
                                const float v,
                                float r_rgba[4])
{
  if (!r_rgba || !std::isfinite(u) || !std::isfinite(v) ||
      corner < 0 || corner >= base_mesh.corners_num)
  {
    return false;
  }
  /* Existing true color values, including painted white, take precedence. */
  if (polypaint_sample(mmd, corner, u, v, r_rgba)) {
    return true;
  }
  float patch[4][4];
  if (!polypaint_base_patch(base_mesh, attribute_name, corner, patch)) {
    return false;
  }
  multires_polypaint_cpu::sample_patch(patch, u, v, r_rgba);
  return true;
}
float (*polypaint_grid_ensure_from_base(MultiresModifierData &mmd,
                                        const Mesh &base_mesh,
                                        const char *attribute_name,
                                        const int corner))[4]
{
  if (corner < 0 || corner >= base_mesh.corners_num || mmd.totlvl < 1 ||
      mmd.totlvl > 13)
  {
    return nullptr;
  }
  /* Preflight topology before any resample: no partial mutation on mismatched meshes. */
  if (mmd.sculpt_color_grids && mmd.sculpt_color_grids_num != base_mesh.corners_num) {
    return nullptr;
  }
  /* A color master older than the geometric maximum must be promoted before any
   * edit. Returning an existing lower-resolution grid here would silently cap
   * subsequent painting at its former level. */
  if (mmd.sculpt_color_grids && mmd.sculpt_color_level < mmd.totlvl) {
    if (!polypaint_resample(mmd, mmd.totlvl)) {
      return nullptr;
    }
    if (polypaint_grid(mmd, corner)) {
      return mmd.sculpt_color_grids[corner].colors;
    }
  }
  /* Existing painted data is authoritative, including explicit white. */
  if (mmd.sculpt_color_grids && polypaint_grid(mmd, corner)) {
    return mmd.sculpt_color_grids[corner].colors;
  }
  /* Delete Higher may leave an independent color master above the geometric
   * maximum. Materialize a previously untouched corner at that stored stride,
   * then let the coarse writer apply its interpolated difference as usual. */
  const int master_level = mmd.sculpt_color_grids ? mmd.sculpt_color_level : mmd.totlvl;
  if (master_level < 1 || master_level > 13) {
    return nullptr;
  }
  float patch[4][4];
  if (!polypaint_base_patch(base_mesh, attribute_name, corner, patch)) {
    return nullptr;
  }
  if (!polypaint_ensure(mmd, master_level, base_mesh.corners_num) ||
      mmd.sculpt_color_level != master_level)
  {
    return nullptr;
  }
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  float (*master)[4] = polypaint_grid_ensure_master(mmd, corner);
  if (!master) {
    return nullptr;
  }
  /* polypaint_grid_ensure_master() is a no-op for a populated grid. Only the
   * newly allocated case reaches here, because it was tested above. */
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      multires_polypaint_cpu::sample_patch(
          patch, float(x) / float(side - 1), float(y) / float(side - 1),
          master[y * side + x]);
    }
  }
  return master;
}
bool original_polypaint_write_coarse_sample(MultiresModifierData &mmd,
                                   const Mesh &base_mesh,
                                   const char *attribute_name,
                                   const int corner,
                                   const int paint_level,
                                   const int x,
                                   const int y,
                                   const float rgba[4])
{
  if (!rgba || paint_level < 1 || paint_level > mmd.totlvl ||
      paint_level > 13 || x < 0 || y < 0)
  {
    return false;
  }
  const int paint_side = subdiv::grid_size_from_level(paint_level);
  if (x >= paint_side || y >= paint_side) {
    return false;
  }
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(rgba[c])) {
      return false;
    }
  }
  float (*master)[4] = polypaint_grid_ensure_from_base(mmd, base_mesh, attribute_name, corner);
  if (!master) {
    return false;
  }
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  const float u = float(x) / float(paint_side - 1);
  const float v = float(y) / float(paint_side - 1);
  float current[4];
  if (!multires_polypaint_cpu::sample_grid(master, side, u, v, current)) {
    return false;
  }
  float delta[4];
  for (int c = 0; c < 4; c++) {
    delta[c] = rgba[c] - current[c];
  }
  return multires_polypaint_cpu::add_coarse_delta(
      master, side, paint_side, x, y, delta);
}
bool polypaint_paint_prepared_sample(float (*master)[4],
                                    const int master_side,
                                    const int paint_side,
                                    const int x,
                                    const int y,
                                    const float rgb[3],
                                    const float influence)
{
  if (!master || !rgb || !multires_polypaint_cpu::valid_side(paint_side) ||
      x < 0 || y < 0 || x >= paint_side || y >= paint_side)
  {
    return false;
  }
  const float u = float(x) / float(paint_side - 1);
  const float v = float(y) / float(paint_side - 1);
  float before[4];
  if (!multires_polypaint_cpu::sample_grid(master, master_side, u, v, before)) {
    return false;
  }
  float delta[4];
  for (int channel = 0; channel < 3; channel++) {
    const float target = before[channel] * (1.0f - influence) + rgb[channel] * influence;
    delta[channel] = target - before[channel];
  }
  /* Retain the original alpha and its non-finite rejection exactly. */
  delta[3] = before[3] - before[3];
  return multires_polypaint_cpu::add_coarse_delta(
      master, master_side, paint_side, x, y, delta);
}
}


#include <cstdio>
#include <cstdlib>
std::atomic<int> slow_writes{0};
namespace blender::bke::multires_layers{
bool polypaint_write_coarse_sample(MultiresModifierData&m,const Mesh&mesh,const char*name,int grid,int level,int x,int y,const float rgba[4]){++slow_writes;return original_polypaint_write_coarse_sample(m,mesh,name,grid,level,x,y,rgba);}}
struct float3{float x=0,y=0,z=0;float operator[](int i)const{return i==0?x:i==1?y:z;}};
template<class T>struct Vector:std::vector<T>{Span<T>as_span()const{return{this};}};
struct Depsgraph{};struct Paint{};struct Sculpt{Paint paint;};struct Brush{};
Brush global_brush;float3 color{.8f,.03f,.6f},secondary{.06f,.3f,.9f};int factor_mode=0;
const Brush*BKE_paint_brush_for_read(const Paint*){return&global_brush;}
float3 BKE_brush_color_get(const Paint*,const Brush*){return color;}
float3 BKE_brush_secondary_color_get(const Paint*,const Brush*){return secondary;}
float BKE_brush_alpha_get(const Paint*,const Brush*){return.7f;}
bool active_color_attribute(const Mesh&m){return m.has_attribute;}
struct Cache{struct Toggle{bool invert=false;}toggle_settings;float bstrength=.8f;struct PB{float flow=.7f;}paint_brush;};
struct SubdivCCG{int grid_size=0,grids_num=8,grid_area=0;};
struct SculptSession{Cache*cache=nullptr;SubdivCCG*subdiv_ccg=nullptr;MultiresModifierData*multires_modifier=nullptr;};
struct Runtime{SculptSession*sculpt_session;};struct Object{Runtime*runtime;Mesh*data;};
struct IndexMaskMemory{};
struct ParallelMode{};namespace exec_mode{ParallelMode grain_size(int){return {};}}
std::atomic<int> parallel_calls{0};
struct IndexMask{
 std::vector<int>indices{0,1};int size()const{return indices.size();}
 template<class F>void foreach_index(F f)const{for(int i:indices)f(i);}
 template<class F>void foreach_index(F f,ParallelMode)const{
  parallel_calls++;std::thread a([&]{for(size_t i=0;i<indices.size();i+=2)f(indices[i]);});
  std::thread b([&]{for(size_t i=1;i<indices.size();i+=2)f(indices[i]);});a.join();b.join();
 }
 template<class F>static IndexMask from_predicate(const IndexMask&old,IndexMaskMemory&,F f){IndexMask out;out.indices.clear();old.foreach_index([&](int i){if(f(i))out.indices.push_back(i);});return out;}
};
namespace blender::threading {
 template<class T>struct EnumerableThreadSpecific {
  std::mutex mutex;std::map<std::thread::id,std::unique_ptr<T>> values;
  T&local(){std::lock_guard<std::mutex>lock(mutex);auto&v=values[std::this_thread::get_id()];if(!v)v=std::make_unique<T>();return *v;}
  struct Iter {typename std::map<std::thread::id,std::unique_ptr<T>>::iterator it;const T&operator*()const{return *it->second;}Iter&operator++(){++it;return *this;}bool operator!=(const Iter&o)const{return it!=o.it;}};
  Iter begin(){return{values.begin()};}Iter end(){return{values.end()};}
 };
}
namespace blender::bke{using ::AttrDomain;namespace multires_layers{bool polypaint_lazy_master_enabled(){return false;}}}
namespace blender::bke::pbvh{
struct GridsNode{std::vector<int>ids;Span<int>grids()const{return{&ids};}};
struct Tree{std::vector<GridsNode>n{{{0,1,2}},{{3,4,5,6,7}}};int tags=0;std::vector<int>last_tag;template<class T>Span<T>nodes()const{return{&n};}void tag_attribute_changed(const IndexMask&mask,const char*){++tags;last_tag=mask.indices;}};
}
blender::bke::pbvh::Tree tree;
namespace blender::bke::object{blender::bke::pbvh::Tree*pbvh_get(Object&){return&tree;}}
template<class T>T id_cast(Mesh*p){return p;}
double BLI_time_now_seconds(){return 0.;}
Span<float3>gather_grids_positions(const SubdivCCG&ccg,Span<int>grids,Vector<float3>&positions){positions.clear();for(int g:grids)for(int i=0;i<ccg.grid_area;i++)positions.push_back({float(g*ccg.grid_area+i),0,0});return{&positions};}
void calc_factors_common_grids(const Depsgraph&,const Brush&,Object&,Span<float3>co,const blender::bke::pbvh::GridsNode&,Vector<float>&f,Vector<float>&d){f.resize(co.size());d.resize(co.size());for(int i=0;i<co.size();i++)f[i]=factor_mode==0?0.f:factor_mode==3?(co[i].x<4?.8f:0.f):factor_mode==1?((int(co[i].x)%7)?0.f:.8f):.4f+float(int(co[i].x)%3)*.2f;}
static void old_paint(const Depsgraph &depsgraph,
                                     const Sculpt &sd,
                                     Object &object,
                                     const IndexMask &node_mask)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_POLYPAINT_CPU");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || !ss.multires_modifier) {
    return;
  }
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double paint_start = trace ? BLI_time_now_seconds() : 0.0;
  MultiresModifierData &mmd = *ss.multires_modifier;
  SubdivCCG &ccg = *ss.subdiv_ccg;
  Mesh &base_mesh = *id_cast<Mesh *>(object.data);
  if (mmd.sculptlvl < 1 || mmd.sculptlvl > mmd.totlvl ||
      ccg.grid_size != bke::subdiv::grid_size_from_level(mmd.sculptlvl) ||
      ccg.grids_num != base_mesh.corners_num ||
      !active_color_attribute(base_mesh))
  {
    return;
  }
  const char *name = base_mesh.active_color_attribute;
  if (!name || name[0] == '\0') {
    return;
  }
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float3 primary = ss.cache->toggle_settings.invert ?
                             BKE_brush_secondary_color_get(&sd.paint, &brush) :
                             BKE_brush_color_get(&sd.paint, &brush);
  const float strength = std::clamp(
      std::abs(ss.cache->bstrength) * BKE_brush_alpha_get(&sd.paint, &brush) *
          ss.cache->paint_brush.flow,
      0.0f,
      1.0f);
  if (strength == 0.0f) {
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  bool changed = false;
  int64_t touched_samples = 0;

  /* The grid sampler and delta writer share the same master storage. Serial
   * execution prevents overlapping coarse-support writes across PBVH nodes. */
  node_mask.foreach_index([&](const int node_index) {
    const Span<int> grids = nodes[node_index].grids();
    const Span<float3> co = gather_grids_positions(ccg, grids, positions);
    calc_factors_common_grids(
        depsgraph, brush, object, co, nodes[node_index], factors, distances);
    for (const int i : co.index_range()) {
      const float influence = std::clamp(factors[i] * strength, 0.0f, 1.0f);
      if (influence <= 0.0f) {
        continue;
      }
      const int local_grid = i / ccg.grid_area;
      const int pixel = i % ccg.grid_area;
      const int grid_index = grids[local_grid];
      const int x = pixel % ccg.grid_size;
      const int y = pixel / ccg.grid_size;
      const float u = float(x) / float(ccg.grid_size - 1);
      const float v = float(y) / float(ccg.grid_size - 1);
      float before[4];
      if (!bke::multires_layers::polypaint_sample_with_base(
              mmd, base_mesh, name, grid_index, u, v, before))
      {
        continue;
      }
      float target[4];
      for (int channel = 0; channel < 3; channel++) {
        target[channel] = before[channel] * (1.0f - influence) +
                          primary[channel] * influence;
      }
      target[3] = before[3];
      const bool wrote = bke::multires_layers::polypaint_write_coarse_sample(
          mmd, base_mesh, name, grid_index, mmd.sculptlvl, x, y, target);
      changed |= wrote;
      touched_samples += int(wrote);
    }
  });
  if (changed) {
    pbvh.tag_attribute_changed(node_mask, name);
  }
  if (trace) {
    std::printf("[POLYPAINT_CPU] paint_level=%d master_level=%d nodes=%lld "
                "touched=%lld changed=%d total=%.3fms\n",
                mmd.sculptlvl, mmd.sculpt_color_level,
                static_cast<long long>(node_mask.size()),
                static_cast<long long>(touched_samples), int(changed),
                (BLI_time_now_seconds() - paint_start) * 1000.0);
  }
}
static void do_paint_brush_grids_cpu_prepared_v02(const Depsgraph &depsgraph,
                                     const Sculpt &sd,
                                     Object &object,
                                     const IndexMask &node_mask)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_POLYPAINT_CPU");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || !ss.multires_modifier) {
    return;
  }
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double paint_start = trace ? BLI_time_now_seconds() : 0.0;
  MultiresModifierData &mmd = *ss.multires_modifier;
  SubdivCCG &ccg = *ss.subdiv_ccg;
  Mesh &base_mesh = *id_cast<Mesh *>(object.data);
  if (mmd.sculptlvl < 1 || mmd.sculptlvl > mmd.totlvl ||
      ccg.grid_size != bke::subdiv::grid_size_from_level(mmd.sculptlvl) ||
      ccg.grids_num != base_mesh.corners_num ||
      !active_color_attribute(base_mesh))
  {
    return;
  }
  const char *name = base_mesh.active_color_attribute;
  if (!name || name[0] == '\0') {
    return;
  }
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float3 primary = ss.cache->toggle_settings.invert ?
                             BKE_brush_secondary_color_get(&sd.paint, &brush) :
                             BKE_brush_color_get(&sd.paint, &brush);
  const float strength = std::clamp(
      std::abs(ss.cache->bstrength) * BKE_brush_alpha_get(&sd.paint, &brush) *
          ss.cache->paint_brush.flow,
      0.0f,
      1.0f);
  if (strength == 0.0f) {
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  bool changed = false;
  int64_t touched_samples = 0;
  const char *prepared_env = std::getenv("BLENDER_SCULPT_POLYPAINT_PREPARED_V2");
  const bool prepared_enabled = prepared_env && prepared_env[0] == '1' && prepared_env[1] == '\0';
  bke::multires_polypaint_cpu::PreparedPaintPlan prepared_plan;
  int64_t prepared_samples = 0, prepared_grids = 0;
  double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
  const double setup_end = trace ? BLI_time_now_seconds() : 0.0;

  /* The grid sampler and delta writer share the same master storage. Serial
   * execution prevents overlapping coarse-support writes across PBVH nodes. */
  node_mask.foreach_index([&](const int node_index) {
    const Span<int> grids = nodes[node_index].grids();
    double stage_start = trace ? BLI_time_now_seconds() : 0.0;
    const Span<float3> co = gather_grids_positions(ccg, grids, positions);
    if (trace) {
      const double now = BLI_time_now_seconds();
      gather_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    calc_factors_common_grids(
        depsgraph, brush, object, co, nodes[node_index], factors, distances);
    if (trace) {
      const double now = BLI_time_now_seconds();
      factors_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    int prepared_grid = -1;
    float (*prepared_master)[4] = nullptr;
    for (const int i : co.index_range()) {
      const float influence = std::clamp(factors[i] * strength, 0.0f, 1.0f);
      if (influence <= 0.0f) {
        continue;
      }
      const int local_grid = i / ccg.grid_area;
      const int pixel = i % ccg.grid_area;
      const int grid_index = grids[local_grid];
      const int x = pixel % ccg.grid_size;
      const int y = pixel / ccg.grid_size;
      if (prepared_grid != grid_index) {
        prepared_grid = grid_index;
        prepared_master = nullptr;
      }
      if (prepared_master) {
        const float brush_rgb[3] = {primary[0], primary[1], primary[2]};
        const bool wrote = prepared_plan.blend(prepared_master, x, y, brush_rgb, influence);
        changed |= wrote;
        touched_samples += int(wrote);
        prepared_samples += int(wrote);
        continue;
      }
      const float u = float(x) / float(ccg.grid_size - 1);
      const float v = float(y) / float(ccg.grid_size - 1);
      float before[4];
      if (!bke::multires_layers::polypaint_sample_with_base(
              mmd, base_mesh, name, grid_index, u, v, before))
      {
        continue;
      }
      float target[4];
      for (int channel = 0; channel < 3; channel++) {
        target[channel] = before[channel] * (1.0f - influence) +
                          primary[channel] * influence;
      }
      target[3] = before[3];
      const bool wrote = bke::multires_layers::polypaint_write_coarse_sample(
          mmd, base_mesh, name, grid_index, mmd.sculptlvl, x, y, target);
      if (wrote && prepared_enabled && bke::multires_layers::polypaint_grid(mmd, grid_index)) {
        const double start = trace ? BLI_time_now_seconds() : 0.0;
        const int master_side = bke::subdiv::grid_size_from_level(mmd.sculpt_color_level);
        if (prepared_plan.prepare(master_side, ccg.grid_size)) {
          prepared_master = mmd.sculpt_color_grids[grid_index].colors;
          prepared_grids++;
        }
        if (trace) prepare_ms += (BLI_time_now_seconds() - start) * 1000.0;
      }
      changed |= wrote;
      touched_samples += int(wrote);
    }
    if (trace) write_ms += (BLI_time_now_seconds() - stage_start) * 1000.0;
  });
  const double notify_start = trace ? BLI_time_now_seconds() : 0.0;
  if (changed) {
    pbvh.tag_attribute_changed(node_mask, name);
  }
  const double paint_end = trace ? BLI_time_now_seconds() : 0.0;
  if (trace) {
    std::printf("[POLYPAINT_CPU] paint_level=%d master_level=%d nodes=%lld "
                "touched=%lld changed=%d total=%.3fms\n",
                mmd.sculptlvl, mmd.sculpt_color_level,
                static_cast<long long>(node_mask.size()),
                static_cast<long long>(touched_samples), int(changed),
                (BLI_time_now_seconds() - paint_start) * 1000.0);
    std::printf("[POLYPAINT_PREPARED_V2] enabled=%d reused_samples=%lld prepared_grids=%lld "
                "setup=%.3fms gather=%.3fms factors=%.3fms write=%.3fms "
                "prepare=%.3fms notify=%.3fms\n",
                int(prepared_enabled), static_cast<long long>(prepared_samples),
                static_cast<long long>(prepared_grids), (setup_end - paint_start) * 1000.0,
                gather_ms, factors_ms, write_ms, prepare_ms,
                (paint_end - notify_start) * 1000.0);
  }
}

static void do_paint_brush_grids_cpu(const Depsgraph &depsgraph,
                                     const Sculpt &sd,
                                     Object &object,
                                     const IndexMask &node_mask)
{
  const auto flag_enabled = [](const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
  };
  if (!flag_enabled("BLENDER_SCULPT_POLYPAINT_PARALLEL_V3") &&
      !flag_enabled("BLENDER_SCULPT_POLYPAINT_UNDO_COPY_V3") &&
      !flag_enabled("BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3"))
  {
    do_paint_brush_grids_cpu_prepared_v02(depsgraph, sd, object, node_mask);
    return;
  }
  const char *enabled = std::getenv("BLENDER_SCULPT_POLYPAINT_CPU");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || !ss.multires_modifier) {
    return;
  }
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double paint_start = trace ? BLI_time_now_seconds() : 0.0;
  MultiresModifierData &mmd = *ss.multires_modifier;
  SubdivCCG &ccg = *ss.subdiv_ccg;
  Mesh &base_mesh = *id_cast<Mesh *>(object.data);
  if (mmd.sculptlvl < 1 || mmd.sculptlvl > mmd.totlvl ||
      ccg.grid_size != bke::subdiv::grid_size_from_level(mmd.sculptlvl) ||
      ccg.grids_num != base_mesh.corners_num ||
      !active_color_attribute(base_mesh))
  {
    return;
  }
  const char *name = base_mesh.active_color_attribute;
  if (!name || name[0] == '\0') {
    return;
  }
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float3 primary = ss.cache->toggle_settings.invert ?
                             BKE_brush_secondary_color_get(&sd.paint, &brush) :
                             BKE_brush_color_get(&sd.paint, &brush);
  const float strength = std::clamp(
      std::abs(ss.cache->bstrength) * BKE_brush_alpha_get(&sd.paint, &brush) *
          ss.cache->paint_brush.flow,
      0.0f,
      1.0f);
  if (strength == 0.0f) {
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const char *parallel_env = std::getenv("BLENDER_SCULPT_POLYPAINT_PARALLEL_V3");
  const bool parallel_requested = parallel_env && parallel_env[0] == '1' && parallel_env[1] == '\0';
  const char *local_env = std::getenv("BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3");
  const bool local_redraw = local_env && local_env[0] == '1' && local_env[1] == '\0';
  const char *prepared_env = std::getenv("BLENDER_SCULPT_POLYPAINT_PREPARED_V2");
  const bool prepared_enabled = prepared_env && prepared_env[0] == '1' && prepared_env[1] == '\0';
  bool parallel_ok = parallel_requested && node_mask.size() > 1 &&
      mmd.sculpt_color_grids && mmd.sculpt_color_grids_num == ccg.grids_num &&
      mmd.sculpt_color_level >= mmd.sculptlvl && mmd.sculpt_color_level <= 13 &&
      (mmd.sculpt_color_level >= mmd.totlvl || bke::multires_layers::polypaint_lazy_master_enabled());
  if (parallel_ok) {
    /* Grid indices, not PBVH node ids, own the writes. Refuse overlapping
     * ownership and malformed shared payloads before any worker is started. */
    std::vector<int> owners(ccg.grids_num, -1);
    std::unordered_set<const void *> payloads;
    node_mask.foreach_index([&](const int node_index) {
      for (const int grid : nodes[node_index].grids()) {
        if (grid < 0 || grid >= ccg.grids_num) {
          parallel_ok = false;
          continue;
        }
        if (owners[grid] >= 0) {
          if (owners[grid] != node_index) parallel_ok = false;
          continue;
        }
        owners[grid] = node_index;
        const void *payload = mmd.sculpt_color_grids[grid].colors;
        if (payload && !payloads.insert(payload).second) parallel_ok = false;
      }
    });
    /* Resolve read-only mesh caches on the main thread before lazy grid
     * materialization. The directory/resolution is already fixed by stroke entry. */
    base_mesh.corner_to_face_map();
    base_mesh.faces();
    base_mesh.attributes().lookup<ColorGeometry4f>(name, bke::AttrDomain::Corner);
  }
  struct LocalWork {
    Vector<float3> positions;
    Vector<float> factors;
    Vector<float> distances;
    bke::multires_polypaint_cpu::PreparedPaintPlan prepared_plan;
    int64_t touched_samples = 0, prepared_samples = 0, prepared_grids = 0;
    double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
  };
  threading::EnumerableThreadSpecific<LocalWork> work;
  std::vector<unsigned char> node_changed(nodes.size(), 0);
  const double setup_end = trace ? BLI_time_now_seconds() : 0.0;
  auto paint_node = [&](const int node_index) {
    LocalWork &tls = work.local();
    auto &positions = tls.positions;
    auto &factors = tls.factors;
    auto &distances = tls.distances;
    auto &prepared_plan = tls.prepared_plan;
    bool changed = false;
    int64_t touched_samples = 0, prepared_samples = 0, prepared_grids = 0;
    double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
    const Span<int> grids = nodes[node_index].grids();
    double stage_start = trace ? BLI_time_now_seconds() : 0.0;
    const Span<float3> co = gather_grids_positions(ccg, grids, positions);
    if (trace) {
      const double now = BLI_time_now_seconds();
      gather_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    calc_factors_common_grids(
        depsgraph, brush, object, co, nodes[node_index], factors, distances);
    if (trace) {
      const double now = BLI_time_now_seconds();
      factors_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    int prepared_grid = -1;
    float (*prepared_master)[4] = nullptr;
    for (const int i : co.index_range()) {
      const float influence = std::clamp(factors[i] * strength, 0.0f, 1.0f);
      if (influence <= 0.0f) {
        continue;
      }
      const int local_grid = i / ccg.grid_area;
      const int pixel = i % ccg.grid_area;
      const int grid_index = grids[local_grid];
      const int x = pixel % ccg.grid_size;
      const int y = pixel / ccg.grid_size;
      if (prepared_grid != grid_index) {
        prepared_grid = grid_index;
        prepared_master = nullptr;
      }
      if (prepared_master) {
        const float brush_rgb[3] = {primary[0], primary[1], primary[2]};
        const bool wrote = prepared_plan.blend(prepared_master, x, y, brush_rgb, influence);
        changed |= wrote;
        touched_samples += int(wrote);
        prepared_samples += int(wrote);
        continue;
      }
      const float u = float(x) / float(ccg.grid_size - 1);
      const float v = float(y) / float(ccg.grid_size - 1);
      float before[4];
      if (!bke::multires_layers::polypaint_sample_with_base(
              mmd, base_mesh, name, grid_index, u, v, before))
      {
        continue;
      }
      float target[4];
      for (int channel = 0; channel < 3; channel++) {
        target[channel] = before[channel] * (1.0f - influence) +
                          primary[channel] * influence;
      }
      target[3] = before[3];
      const bool wrote = bke::multires_layers::polypaint_write_coarse_sample(
          mmd, base_mesh, name, grid_index, mmd.sculptlvl, x, y, target);
      if (wrote && prepared_enabled && bke::multires_layers::polypaint_grid(mmd, grid_index)) {
        const double start = trace ? BLI_time_now_seconds() : 0.0;
        const int master_side = bke::subdiv::grid_size_from_level(mmd.sculpt_color_level);
        if (prepared_plan.prepare(master_side, ccg.grid_size)) {
          prepared_master = mmd.sculpt_color_grids[grid_index].colors;
          prepared_grids++;
        }
        if (trace) prepare_ms += (BLI_time_now_seconds() - start) * 1000.0;
      }
      changed |= wrote;
      touched_samples += int(wrote);
    }
    if (trace) write_ms += (BLI_time_now_seconds() - stage_start) * 1000.0;
    tls.touched_samples += touched_samples;
    tls.prepared_samples += prepared_samples;
    tls.prepared_grids += prepared_grids;
    tls.gather_ms += gather_ms;
    tls.factors_ms += factors_ms;
    tls.write_ms += write_ms;
    tls.prepare_ms += prepare_ms;
    node_changed[node_index] = changed;
  };
  if (parallel_ok) {
    node_mask.foreach_index(paint_node, exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(paint_node);
  }
  int64_t touched_samples = 0, prepared_samples = 0, prepared_grids = 0;
  double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
  int workers = 0;
  for (const LocalWork &tls : work) {
    touched_samples += tls.touched_samples;
    prepared_samples += tls.prepared_samples;
    prepared_grids += tls.prepared_grids;
    gather_ms += tls.gather_ms;
    factors_ms += tls.factors_ms;
    write_ms += tls.write_ms;
    prepare_ms += tls.prepare_ms;
    workers++;
  }
  bool changed = false;
  node_mask.foreach_index([&](const int node_index) { changed |= bool(node_changed[node_index]); });
  const double notify_start = trace ? BLI_time_now_seconds() : 0.0;
  int64_t redraw_nodes = 0;
  if (changed) {
    if (local_redraw) {
      IndexMaskMemory memory;
      const IndexMask redraw = IndexMask::from_predicate(node_mask, memory,
          [&](const int i) { return bool(node_changed[i]); });
      pbvh.tag_attribute_changed(redraw, name);
      redraw_nodes = redraw.size();
    }
    else {
      pbvh.tag_attribute_changed(node_mask, name);
      redraw_nodes = node_mask.size();
    }
  }
  const double paint_end = trace ? BLI_time_now_seconds() : 0.0;
  if (trace) {
    std::printf("[POLYPAINT_CPU] paint_level=%d master_level=%d nodes=%lld "
                "touched=%lld changed=%d total=%.3fms\n",
                mmd.sculptlvl, mmd.sculpt_color_level,
                static_cast<long long>(node_mask.size()),
                static_cast<long long>(touched_samples), int(changed),
                (BLI_time_now_seconds() - paint_start) * 1000.0);
    std::printf("[POLYPAINT_PARALLEL_V3] requested=%d eligible=%d workers=%d redraw_local=%d redraw_nodes=%lld\n",
                int(parallel_requested), int(parallel_ok), workers, int(local_redraw),
                static_cast<long long>(redraw_nodes));
    std::printf("[POLYPAINT_PREPARED_V2] enabled=%d reused_samples=%lld prepared_grids=%lld "
                "setup=%.3fms gather=%.3fms factors=%.3fms write=%.3fms "
                "prepare=%.3fms notify=%.3fms\n",
                int(prepared_enabled), static_cast<long long>(prepared_samples),
                static_cast<long long>(prepared_grids), (setup_end - paint_start) * 1000.0,
                gather_ms, factors_ms, write_ms, prepare_ms,
                (paint_end - notify_start) * 1000.0);
  }
}
struct Fixture{Mesh mesh;MultiresModifierData m;Cache cache;SubdivCCG ccg;SculptSession ss;Runtime runtime;Object object;
Fixture(int level,int master,int kind):ss{&cache,&ccg,&m},runtime{&ss},object{&runtime,&mesh}{m.sculptlvl=level;m.totlvl=5;ccg.grid_size=blender::bke::subdiv::grid_size_from_level(level);ccg.grid_area=ccg.grid_size*ccg.grid_size;
 for(int i=0;i<8;i++)mesh.colors[i]={.17f+i*.03f,.28f,.41f,.37f};
 cache.toggle_settings.invert=kind&1;if(kind==3)mesh.has_attribute=false;
 if(kind==4){assert(blender::bke::multires_layers::polypaint_ensure(m,5,8));}
 if(kind==1||kind==2){assert(blender::bke::multires_layers::polypaint_ensure(m,master,8));int side=blender::bke::subdiv::grid_size_from_level(master);for(int g=0;g<8;g++){auto*p=blender::bke::multires_layers::polypaint_grid_ensure_master(m,g);for(int i=0;i<side*side;i++){p[i][0]=1.3f+i*.0001f;p[i][1]=-.2f;p[i][2]=.37f;p[i][3]=.7f;}}}
}
~Fixture(){blender::bke::multires_layers::polypaint_clear(m);}
std::vector<float>snapshot(){std::vector<float>r{float(m.sculpt_color_level),float(m.sculpt_color_grids_num)};for(int g=0;g<m.sculpt_color_grids_num;g++){auto&grid=m.sculpt_color_grids[g];r.push_back(grid.level);r.push_back(grid.totcolor);if(grid.colors)r.insert(r.end(),grid.colors[0],grid.colors[0]+grid.totcolor*4);}return r;}
};

int main(){setenv("BLENDER_SCULPT_POLYPAINT_CPU","1",1);unsetenv("BLENDER_LAYER_RESP_TRACE");setenv("BLENDER_SCULPT_POLYPAINT_PREPARED_V2","1",1);Sculpt sculpt;Depsgraph deps;IndexMask mask;
 int cases=0,workers=0,fallbacks=0,local_updates=0;
 for(int level=1;level<=5;level++)for(int master:{2,5,6})for(int kind=0;kind<5;kind++)for(int mode=0;mode<4;mode++)for(int overlap=0;overlap<2;overlap++)for(int parallel=0;parallel<2;parallel++)for(int local=0;local<2;local++){
  factor_mode=mode;Fixture a(level,master,kind),b(level,master,kind);
  tree.n={{{0,1,2}},{{3,4,5,6,7}}};if(overlap)tree.n[1].ids.push_back(2);
  setenv("BLENDER_SCULPT_POLYPAINT_PARALLEL_V3",parallel?"1":"0",1);setenv("BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3",local?"1":"0",1);
  for(int dab=0;dab<2;dab++){
   tree.tags=0;old_paint(deps,sculpt,a.object,mask);int oldtags=tree.tags;
   parallel_calls=0;tree.tags=0;tree.last_tag.clear();do_paint_brush_grids_cpu(deps,sculpt,b.object,mask);
   assert(a.snapshot()==b.snapshot());assert(tree.tags==oldtags);
   if(overlap||!parallel)assert(parallel_calls==0);
   if(parallel_calls)workers++;else fallbacks++;
   if(local&&mode==3&&oldtags){assert(tree.last_tag==std::vector<int>{0});local_updates++;}
   cases++;
  }
 }
 assert(live_arrays==0&&workers>0&&fallbacks>0&&local_updates>0);
 std::printf("PASS %d actual brush-loop cases; two real worker threads, worker runs=%d serial/fallback=%d local redraw=%d; identical color values; PBVH/cache APIs modeled\n",cases,workers,fallbacks,local_updates);
}
