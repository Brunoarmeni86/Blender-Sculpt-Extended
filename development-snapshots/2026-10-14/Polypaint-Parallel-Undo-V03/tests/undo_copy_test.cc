#ifdef NDEBUG
#error Assertions required
#endif

// SPDX-License-Identifier: GPL-2.0-or-later
// Actual BKE Polypaint functions and Color Undo capture/restore; native scene/allocator stubbed.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <limits>
#include "polypaint_cpu_math.hh"
int live_arrays=0;
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
struct MultiresModifierData{MultiresColorGrid*sculpt_color_grids=nullptr;int sculpt_color_grids_num=0,sculpt_color_level=0,sculptlvl=3,totlvl=3;char sculpt_color_attribute[68]="";};
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



#include <cstdlib>
#include <cstdio>
#define STREQ(a,b) (std::strcmp(a,b)==0)
namespace blender::bke::multires_layers{
 bool polypaint_lazy_master_enabled(){return true;}
 bool polypaint_attribute_activate(MultiresModifierData&,const char*){return true;}
}
struct float4{float v[4];float4(){for(float&x:v)x=std::numeric_limits<float>::quiet_NaN();}explicit float4(float x){for(float&a:v)a=x;}explicit float4(const float*p){std::copy(p,p+4,v);}float&operator[](int c){return v[c];}const float&operator[](int c)const{return v[c];}};
template<class T>struct Array{std::vector<T>v;void reinitialize(size_t n){v=std::vector<T>(n);}void fill(const T&x){std::fill(v.begin(),v.end(),x);}size_t size()const{return v.size();}T*data(){return v.data();}const T*data()const{return v.data();}T&operator[](int i){return v[i];}const T&operator[](int i)const{return v[i];}};
struct Grids{std::vector<int>v;int size()const{return v.size();}int operator[](int i)const{return v[i];}IndexRange index_range()const{return IndexRange(size());}auto begin()const{return v.begin();}auto end()const{return v.end();}};
struct Bits{std::vector<unsigned char>v;void resize(size_t n){v.resize(n);}size_t size()const{return v.size();}struct Ref{unsigned char*p;void set(bool b){*p=b;}operator bool()const{return *p!=0;}};Ref operator[](int i){return{&v[i]};}bool operator[](int i)const{return v[i]!=0;}};
struct Node{Grids grids;Array<float4>col;int grid_color_side=0;Bits grid_color_present;};
struct SubdivCCG{int grids_num=8;};struct SculptSession{MultiresModifierData*multires_modifier;SubdivCCG*subdiv_ccg;};struct Runtime{SculptSession*sculpt_session;};struct Object{Runtime*runtime;Mesh*data;};
template<class T>T id_cast(Mesh*p){return static_cast<T>(p);}
struct StepData{std::vector<std::unique_ptr<Node>>nodes;};
void capture(const Object&object,Node&unode){const SculptSession&ss=*object.runtime->sculpt_session;const SubdivCCG&subdiv_ccg=*ss.subdiv_ccg;do{
      MultiresModifierData *mmd = ss.multires_modifier;
      /* The first color stroke may materialize a previously implicit grid.
       * Capture presence separately, so Undo returns to the base attribute. */
      if (!mmd || mmd->totlvl < 1 || mmd->totlvl > 13) {
        break;
      }
      if (mmd->sculpt_color_grids &&
          mmd->sculpt_color_grids_num != subdiv_ccg.grids_num)
      {
        break;
      }
      if (mmd->sculpt_color_grids && mmd->sculpt_color_level < mmd->totlvl &&
          (bke::multires_layers::polypaint_lazy_master_enabled() ?
               mmd->sculpt_color_level < mmd->sculptlvl :
               !bke::multires_layers::polypaint_resample(*mmd, mmd->totlvl)))
      {
        break;
      }
      const int master_level = mmd->sculpt_color_grids ? mmd->sculpt_color_level :
                                                             mmd->totlvl;
      unode.grid_color_side = bke::subdiv::grid_size_from_level(master_level);
      const int master_area = unode.grid_color_side * unode.grid_color_side;
      unode.col.reinitialize(unode.grids.size() * master_area);
      const char *fast_env = std::getenv("BLENDER_SCULPT_POLYPAINT_UNDO_COPY_V3");
      const bool fast_copy = fast_env && fast_env[0] == '1' && fast_env[1] == '\0';
      /* Absent slots are never read: restore fills them before Redo can read
       * them, and original-color lookup rejects any missing presence bit.
       * Dense slots are overwritten in full. Avoid touching every page twice. */
      if (!fast_copy) unode.col.fill(float4(0.0f));
      unode.grid_color_present.resize(unode.grids.size());
      for (const int i : unode.grids.index_range()) {
        const float (*colors)[4] = bke::multires_layers::polypaint_grid(*mmd, unode.grids[i]);
        if (!colors) {
          unode.grid_color_present[i].set(false);
          continue;
        }
        unode.grid_color_present[i].set(true);
        if (fast_copy) {
          static_assert(sizeof(float4) == sizeof(float[4]));
          std::memcpy(unode.col.data() + i * master_area, colors,
                      size_t(master_area) * sizeof(float4));
        }
        else {
          for (int p = 0; p < master_area; p++) {
            unode.col[i * master_area + p] = float4(colors[p]);
          }
        }
      }
      break;
}while(false);}
static void restore_color_grids(Object &object, StepData &step_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  MultiresModifierData *mmd = ss.multires_modifier;
  SubdivCCG *ccg = ss.subdiv_ccg;
  /* POLYPAINT_REDO_MASTER_V1: memfile Undo can remove the entire sparse
   * master table. Redo still owns the captured colors in its Sculpt nodes.
   * Recreate the table at the captured master level, never the Sculpt level. */
  if (!mmd || !ccg) {
    return;
  }
  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  if (mmd->sculpt_color_attribute[0] && mesh.active_color_attribute &&
      !STREQ(mmd->sculpt_color_attribute, mesh.active_color_attribute) &&
      !bke::multires_layers::polypaint_attribute_activate(*mmd, mesh.active_color_attribute)) {
    return;
  }
  if (!mmd->sculpt_color_grids) {
    int saved_level = 0;
    bool has_saved_colors = false;
    for (const std::unique_ptr<Node> &unode : step_data.nodes) {
      int level = 0;
      for (int candidate = 1; candidate <= 13; candidate++) {
        if (bke::subdiv::grid_size_from_level(candidate) == unode->grid_color_side) {
          level = candidate;
          break;
        }
      }
      if (level == 0 || (saved_level != 0 && saved_level != level) ||
          unode->grid_color_present.size() != unode->grids.size() ||
          unode->col.size() != unode->grids.size() *
                                   unode->grid_color_side * unode->grid_color_side)
      {
        return;
      }
      saved_level = level;
      for (const int i : unode->grids.index_range()) {
        if (unode->grids[i] < 0 || unode->grids[i] >= ccg->grids_num) {
          return;
        }
        has_saved_colors |= bool(unode->grid_color_present[i]);
      }
    }
    /* Nothing to restore: preserve implicit base-attribute colors. */
    if (!has_saved_colors) {
      return;
    }
    if (!bke::multires_layers::polypaint_ensure(*mmd, saved_level, ccg->grids_num)) {
      return;
    }
    const char *trace = std::getenv("BLENDER_LAYER_RESP_TRACE");
    if (trace && trace[0] && trace[0] != '0') {
      std::printf("[POLYPAINT_UNDO] event=MASTER_RECREATE level=%d grids=%d\n",
                  saved_level, ccg->grids_num);
      std::fflush(stdout);
    }
  }
  if (!mmd->sculpt_color_grids || mmd->sculpt_color_level < 1 ||
      mmd->sculpt_color_level > 13 ||
      mmd->sculpt_color_grids_num != ccg->grids_num)
  {
    return;
  }
  for (std::unique_ptr<Node> &unode : step_data.nodes) {
    const int side = unode->grid_color_side;
    if (side < 2 || side != bke::subdiv::grid_size_from_level(mmd->sculpt_color_level) ||
        unode->grid_color_present.size() != unode->grids.size())
    {
      continue;
    }
    const int area = side * side;
    if (unode->col.size() != unode->grids.size() * area) {
      continue;
    }
    for (const int i : unode->grids.index_range()) {
      const int grid_index = unode->grids[i];
      float4 *stored = unode->col.data() + i * area;
      const float (*current)[4] = bke::multires_layers::polypaint_grid(*mmd, grid_index);
      const bool had_color = unode->grid_color_present[i];
      const bool has_color = current != nullptr;
      if (had_color && has_color) {
        float (*writable)[4] = bke::multires_layers::polypaint_grid_ensure_master(*mmd,
                                                                                   grid_index);
        if (!writable) {
          continue;
        }
        for (int p = 0; p < area; p++) {
          for (int c = 0; c < 4; c++) {
            std::swap(writable[p][c], stored[p][c]);
          }
        }
      }
      else if (!had_color && has_color) {
        for (int p = 0; p < area; p++) {
          stored[p] = float4(current[p]);
        }
        if (!bke::multires_layers::polypaint_grid_discard(*mmd, grid_index)) {
          continue;
        }
      }
      else if (had_color && !has_color) {
        float (*created)[4] = bke::multires_layers::polypaint_grid_ensure_master(*mmd,
                                                                                  grid_index);
        if (!created) {
          continue;
        }
        for (int p = 0; p < area; p++) {
          for (int c = 0; c < 4; c++) {
            created[p][c] = stored[p][c];
          }
        }
      }
      unode->grid_color_present[i].set(has_color);
    }
  }
}


struct Fixture{Mesh mesh;MultiresModifierData m;SubdivCCG ccg;SculptSession ss;Runtime runtime;Object object;StepData step;
 Fixture(int level):ss{&m,&ccg},runtime{&ss},object{&runtime,&mesh}{m.totlvl=level;m.sculptlvl=level;assert(blender::bke::multires_layers::polypaint_ensure(m,level,8));}
 ~Fixture(){blender::bke::multires_layers::polypaint_clear(m);}
 void set(int bits,float value){int side=blender::bke::subdiv::grid_size_from_level(m.totlvl);for(int g=0;g<8;g++){if(bits&(1<<g)){auto*p=blender::bke::multires_layers::polypaint_grid_ensure_master(m,g);for(int i=0;i<side*side;i++){p[i][0]=value+g*.01f;p[i][1]=.2f;p[i][2]=-.3f;p[i][3]=.7f;}}else blender::bke::multires_layers::polypaint_grid_discard(m,g);}}
 std::vector<float>snapshot(){std::vector<float>r;for(int g=0;g<8;g++){const auto*p=blender::bke::multires_layers::polypaint_grid(m,g);r.push_back(p?1.f:0.f);if(p){int side=blender::bke::subdiv::grid_size_from_level(m.sculpt_color_level);r.insert(r.end(),p[0],p[0]+side*side*4);}}return r;}
 void take(bool fast,bool partial){auto n=std::make_unique<Node>();n->grids.v=partial?std::vector<int>{0,1,3,7}:std::vector<int>{0,1,2,3,4,5,6,7};setenv("BLENDER_SCULPT_POLYPAINT_UNDO_COPY_V3",fast?"1":"0",1);capture(object,*n);step.nodes.push_back(std::move(n));}
};
int main(){unsetenv("BLENDER_LAYER_RESP_TRACE");int cases=0,poisoned=0;
for(int level=1;level<=6;level++)for(int before:{0,1,3,127,255})for(int after:{0,2,5,63,255})for(bool partial:{false,true}){
 Fixture a(level),b(level);a.set(before,.2f);b.set(before,.2f);a.take(false,partial);b.take(true,partial);
 for(int i=0;i<b.step.nodes[0]->grids.size();i++)if(!b.step.nodes[0]->grid_color_present[i]){assert(std::isnan(b.step.nodes[0]->col[i*b.step.nodes[0]->grid_color_side*b.step.nodes[0]->grid_color_side][0]));poisoned++;}
 a.set(after,.8f);b.set(after,.8f);auto redo=a.snapshot();
 for(int cycle=0;cycle<12;cycle++){restore_color_grids(a.object,a.step);restore_color_grids(b.object,b.step);assert(a.snapshot()==b.snapshot());for(float v:b.snapshot())assert(std::isfinite(v));if(cycle%2)assert(a.snapshot()==redo);cases++;}
}
assert(live_arrays==0&&poisoned>0);std::printf("PASS %d actual capture/restore cases; %d poisoned implicit slots never become visible, unchanged alpha and Undo/Redo; scene/Array/allocator modeled\n",cases,poisoned);
}
