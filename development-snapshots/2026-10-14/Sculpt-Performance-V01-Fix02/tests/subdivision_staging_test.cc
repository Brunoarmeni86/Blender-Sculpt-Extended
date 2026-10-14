
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
std::unordered_map<void*,size_t>allocations;size_t bytes=0,peak_bytes=0;
template<class T>T* MEM_new_array_zeroed(int n,const char*){T*p=new T[n]{};allocations[p]=sizeof(T)*n;bytes+=sizeof(T)*n;peak_bytes=std::max(peak_bytes,bytes);return p;}
template<class T>void MEM_delete(T*p){if(p){assert(allocations.count(p));bytes-=allocations[p];allocations.erase(p);delete[]p;}}
#define MEM_SAFE_DELETE(p) do {MEM_delete(p);p=nullptr;}while(0)
using BLI_bitmap=uint32_t;
#define BLI_BITMAP_NEW(n,label) MEM_new_array_zeroed<BLI_bitmap>(((n)+31)/32,label)
#define BLI_BITMAP_TEST_BOOL(p,i) (((p)[(i)/32]>>((i)%32))&1u)
#define BLI_BITMAP_SET(p,i,v) do {if(v)(p)[(i)/32]|=1u<<((i)%32);else(p)[(i)/32]&=~(1u<<((i)%32));}while(0)
#define BLI_assert(v) assert(v)
template<class F>struct Defer{F f;~Defer(){f();}};
template<class F>Defer(F)->Defer<F>;
#define JOIN2(a,b) a##b
#define JOIN(a,b) JOIN2(a,b)
#define BLI_SCOPED_DEFER(f) Defer JOIN(defer_,__LINE__){f}
struct MDisps{float(*disps)[3]=nullptr;BLI_bitmap*hidden=nullptr;int level=0,totdisp=0;};
struct GridPaintMask{float*data=nullptr;int level=0;};
struct ID{int users=1;};
struct Mesh{ID id;int corners_num=8,verts_num=2;struct CD{MDisps*grids=nullptr;GridPaintMask*masks=nullptr;}corner_data;struct Attr{bool contains(const char*)const{return false;}};Attr attributes(){return{};}};
struct MultiresSculptLayer{MDisps*disps=nullptr;int level=0,grids_num=0;float(*base_deltas)[3]=nullptr;int base_verts_num=0;float strength=.65f;int flag=1,group_id=7;std::string name="Layer";};
struct Layers:std::vector<MultiresSculptLayer>{bool is_empty()const{return empty();}int count()const{return int(size());}};
struct ModifierData{};
struct MultiresModifierData{ModifierData modifier;Layers sculpt_layers;int totlvl=2,lvl=2,sculptlvl=2,renderlvl=2,sculpt_layer_recording=1;void*sculpt_face_set_grids=nullptr;};
struct Object{Mesh*data;};template<class T>T id_cast(Mesh*m){return m;}
enum{CD_MDISPS=1,CD_GRID_PAINT_MASK=2};enum class MultiresSubdivideModeType{CatmullClark,Simple,Linear};
#define ID_REAL_USERS(id) ((id)->users)
void*CustomData_get_layer(const Mesh::CD*d,int type){return type==CD_MDISPS?(void*)d->grids:(void*)d->masks;}
void*CustomData_get_layer_for_write(Mesh::CD*d,int type,int){return CustomData_get_layer(d,type);}
bool opt=false,keep_empty=true,fail_rebuild=false;int fail_call=0,calls=0,live_meshes=0,max_meshes=0,rebuilds=0;
const char*BLI_getenv(const char*name){if(std::string(name)=="BLENDER_SCULPT_SUBDIVIDE_COMPACT")return opt?"1":"0";if(std::string(name)=="BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE")return keep_empty?"1":"0";return nullptr;}
double BLI_time_now_seconds(){return 0;}
void BKE_modifier_set_error(Object*,ModifierData*,const char*){}
bool multires_force_sculpt_rebuild(Object*){++rebuilds;return !fail_rebuild;}
void multires_set_tot_level(Object*,MultiresModifierData*m,int l){m->totlvl=m->lvl=m->sculptlvl=m->renderlvl=l;}
namespace bke{namespace subdiv{int grid_size_from_level(int l){assert(l>=1&&l<=13);return(1<<(l-1))+1;}}
namespace multires_layers{
int invalidations=0;void weight_hot_delta_cache_invalidate(const MultiresSculptLayer*){++invalidations;}
bool faceset_ensure_from_mesh(MultiresModifierData&,Mesh&,int){return true;}void faceset_resample_up(MultiresModifierData&,int){}
bool base_remap_after_topology_edit(MultiresModifierData&,Mesh&){return true;}bool commit(Object&){return true;}
void initialize_layer_grids(MultiresModifierData&,int,int){}void ensure_corner_ids(MultiresModifierData&,Mesh&){}
bool remap_after_topology_edit(MultiresModifierData&,Mesh&){return true;}
bool compatible(const MultiresModifierData&m,const Mesh&mesh){int side=subdiv::grid_size_from_level(m.totlvl);for(auto&l:m.sculpt_layers){if(!l.disps||l.level!=m.totlvl||l.grids_num!=mesh.corners_num)return false;for(int c=0;c<l.grids_num;c++)if(l.disps[c].level!=l.level||l.disps[c].totdisp!=side*side)return false;}return true;}
float(*grid_ensure(MultiresSculptLayer&l,int c))[3]{auto&g=l.disps[c];if(!g.disps)g.disps=MEM_new_array_zeroed<float[3]>(g.totdisp,"materialized");return g.disps;}
}}
void free_mesh(Mesh*m){for(int c=0;c<m->corners_num;c++){MEM_delete(m->corner_data.grids[c].disps);MEM_delete(m->corner_data.grids[c].hidden);MEM_delete(m->corner_data.masks[c].data);}MEM_delete(m->corner_data.grids);MEM_delete(m->corner_data.masks);delete m;}
void BKE_id_free(void*,Mesh*m){--live_meshes;free_mesh(m);}
void multiresModifier_subdivide_to_level_base(Object*,MultiresModifierData*,int,MultiresSubdivideModeType){}

static bool multires_preserve_retained_samples(const MDisps &base,
                                               const MDisps *layer,
                                               MDisps &result)
{
  if (base.level < 1 || base.level > 13 || result.level <= base.level ||
      result.level > 13 || !base.disps || !result.disps)
  {
    return false;
  }
  const int old_side = bke::subdiv::grid_size_from_level(base.level);
  const int new_side = bke::subdiv::grid_size_from_level(result.level);
  if (base.totdisp != old_side * old_side || result.totdisp != new_side * new_side ||
      (new_side - 1) % (old_side - 1) != 0 ||
      (layer && (layer->level != base.level || layer->totdisp != base.totdisp)))
  {
    return false;
  }
  const int stride = (new_side - 1) / (old_side - 1);
  for (int y = 0; y < old_side; y++) {
    for (int x = 0; x < old_side; x++) {
      const int src = y * old_side + x;
      const int dst = y * stride * new_side + x * stride;
      for (int axis = 0; axis < 3; axis++) {
        const float delta = layer && layer->disps ? layer->disps[src][axis] : 0.0f;
        const float value = base.disps[src][axis] + delta;
        if (!std::isfinite(base.disps[src][axis]) || !std::isfinite(delta) ||
            !std::isfinite(value))
        {
          return false;
        }
        result.disps[dst][axis] = value;
      }
    }
  }
  return true;
}

// Geometry backend stub, shared by both pipelines. The actual retained-sample
// helper constrains old samples after this deterministic interpolation.
static Mesh*multires_subdivide_layer_surface(Object*o,const MultiresModifierData&m,const MultiresSculptLayer*l,int level,MultiresSubdivideModeType mode){
 ++calls;if(calls==fail_call)return nullptr;auto*mesh=new Mesh;mesh->corners_num=o->data->corners_num;
 ++live_meshes;max_meshes=std::max(max_meshes,live_meshes);int ns=bke::subdiv::grid_size_from_level(level),os=bke::subdiv::grid_size_from_level(m.totlvl);
 mesh->corner_data.grids=MEM_new_array_zeroed<MDisps>(mesh->corners_num,"grids");mesh->corner_data.masks=MEM_new_array_zeroed<GridPaintMask>(mesh->corners_num,"masks");
 for(int c=0;c<mesh->corners_num;c++){auto&g=mesh->corner_data.grids[c];g.level=level;g.totdisp=ns*ns;g.disps=MEM_new_array_zeroed<float[3]>(g.totdisp,"disps");auto&mask=mesh->corner_data.masks[c];mask.level=level;mask.data=MEM_new_array_zeroed<float>(g.totdisp,"mask");
  for(int y=0;y<ns;y++)for(int x=0;x<ns;x++){int dst=y*ns+x;float u=float(x)*(os-1)/(ns-1),v=float(y)*(os-1)/(ns-1);int x0=int(u),y0=int(v),x1=std::min(x0+1,os-1),y1=std::min(y0+1,os-1);float fx=u-x0,fy=v-y0;
   for(int a=0;a<3;a++){auto sample=[&](int i){return o->data->corner_data.grids[c].disps[i][a]+(l&&l->disps[c].disps?l->disps[c].disps[i][a]:0.0f);};float aa=sample(y0*os+x0)*(1-fx)+sample(y0*os+x1)*fx;float bb=sample(y1*os+x0)*(1-fx)+sample(y1*os+x1)*fx;g.disps[dst][a]=aa*(1-fy)+bb*fy;if(!std::isfinite(g.disps[dst][a])){BKE_id_free(nullptr,mesh);return nullptr;}}
   mask.data[dst]=float(dst+c)*.03f;
  }
  if(mode==MultiresSubdivideModeType::CatmullClark&&!multires_preserve_retained_samples(o->data->corner_data.grids[c],l?&l->disps[c]:nullptr,g)){BKE_id_free(nullptr,mesh);return nullptr;}
  if(o->data->corner_data.grids[c].hidden){g.hidden=BLI_BITMAP_NEW(g.totdisp,"hidden");for(int y=0;y<ns;y++)for(int x=0;x<ns;x++){int sx=int(std::round(float(x)*(os-1)/(ns-1))),sy=int(std::round(float(y)*(os-1)/(ns-1)));BLI_BITMAP_SET(g.hidden,y*ns+x,BLI_BITMAP_TEST_BOOL(o->data->corner_data.grids[c].hidden,sy*os+sx));}}
 }
 return mesh;
}

static bool multires_stage_subdivided_layer(const MultiresSculptLayer &layer,
                                           const MDisps *base,
                                           MDisps *result,
                                           const int grids_num,
                                           const int new_level,
                                           const bool allow_sparse = false)
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
    for (int i = 0; i < grid.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        const float delta = grid.disps[i][axis] - base[corner].disps[i][axis];
        if (!std::isfinite(delta)) {
          return false;
        }
        grid.disps[i][axis] = delta;
      }
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
static void multires_publish_subdivided_layer(MultiresSculptLayer &layer,
                                             MDisps *result,
                                             const int new_level)
{
  for (int corner = 0; corner < layer.grids_num; corner++) {
    std::swap(layer.disps[corner], result[corner]);
  }
  layer.level = new_level;
}
static bool multires_layer_can_keep_empty_subdivide(const MultiresSculptLayer &layer)
{
  if (layer.level <= 0 || layer.level > 13 || layer.disps == nullptr || layer.grids_num <= 0) {
    return false;
  }
  if (layer.base_deltas != nullptr) {
    for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
      const float *delta = layer.base_deltas[vertex];
      if (delta[0] != 0.0f || delta[1] != 0.0f || delta[2] != 0.0f) {
        return false;
      }
    }
  }
  const int side = bke::subdiv::grid_size_from_level(layer.level);
  for (int grid = 0; grid < layer.grids_num; grid++) {
    const MDisps &data = layer.disps[grid];
    if (data.disps != nullptr || data.hidden != nullptr || data.level != layer.level ||
        data.totdisp != side * side)
    {
      return false;
    }
  }
  return true;
}
static void multires_publish_empty_subdivided_layer(MultiresSculptLayer &layer,
                                                   const int new_level)
{
  const int side = bke::subdiv::grid_size_from_level(new_level);
  for (int grid = 0; grid < layer.grids_num; grid++) {
    BLI_assert(layer.disps[grid].disps == nullptr && layer.disps[grid].hidden == nullptr);
    layer.disps[grid].level = new_level;
    layer.disps[grid].totdisp = side * side;
  }
  layer.level = new_level;
}
static bool multires_compact_layer_is_empty(const MultiresSculptLayer &layer)
{
  if (layer.level < 1 || layer.level > 13 || !layer.disps || layer.grids_num <= 0) {
    return false;
  }
  if (layer.base_deltas) {
    for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
      for (int axis = 0; axis < 3; axis++) {
        if (layer.base_deltas[vertex][axis] != 0.0f) {
          return false;
        }
      }
    }
  }
  const int side = bke::subdiv::grid_size_from_level(layer.level);
  for (int corner = 0; corner < layer.grids_num; corner++) {
    const MDisps &grid = layer.disps[corner];
    if (grid.level != layer.level || grid.totdisp != side * side || grid.hidden) {
      return false;
    }
    for (int i = 0; grid.disps && i < grid.totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        if (grid.disps[i][axis] != 0.0f) {
          return false;
        }
      }
    }
  }
  return true;
}
struct MultiresCompactSubdivisionStage {
  int corners_num;
  std::vector<std::pair<MultiresSculptLayer *, MDisps *>> layers;

  explicit MultiresCompactSubdivisionStage(const int corners_num) : corners_num(corners_num) {}
  MultiresCompactSubdivisionStage(const MultiresCompactSubdivisionStage &) = delete;
  MultiresCompactSubdivisionStage &operator=(const MultiresCompactSubdivisionStage &) = delete;
  ~MultiresCompactSubdivisionStage()
  {
    for (const auto &[layer, grids] : layers) {
      (void)layer;
      for (int corner = 0; corner < corners_num; corner++) {
        MEM_SAFE_DELETE(grids[corner].disps);
        MEM_SAFE_DELETE(grids[corner].hidden);
      }
      MEM_delete(grids);
    }
  }
};
static bool multires_subdivide_compact_layers(Object *object,
                                              MultiresModifierData *mmd,
                                              const int top_level,
                                              const MultiresSubdivideModeType mode)
{
  const double started = BLI_time_now_seconds();
  Mesh *mesh = id_cast<Mesh *>(object->data);
  std::vector<MultiresSculptLayer *> empty;
  empty.reserve(mmd->sculpt_layers.count());
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (multires_compact_layer_is_empty(layer)) {
      empty.push_back(&layer);
    }
  }
  Mesh *base = multires_subdivide_layer_surface(object, *mmd, nullptr, top_level, mode);
  BLI_SCOPED_DEFER([&]() { if (base) { BKE_id_free(nullptr, base); } });
  MultiresCompactSubdivisionStage staged(mesh->corners_num);
  staged.layers.reserve(mmd->sculpt_layers.count());
  bool valid = base != nullptr;
  const MDisps *base_grids = base ? static_cast<const MDisps *>(
      CustomData_get_layer(&base->corner_data, CD_MDISPS)) : nullptr;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (!valid) {
      break;
    }
    if (std::find(empty.begin(), empty.end(), &layer) != empty.end()) {
      continue;
    }
    Mesh *surface = multires_subdivide_layer_surface(object, *mmd, &layer, top_level, mode);
    if (!surface) {
      valid = false;
      break;
    }
    MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &surface->corner_data, CD_MDISPS, surface->corners_num));
    valid = multires_stage_subdivided_layer(
        layer, base_grids, grids, mesh->corners_num, top_level, true);
    if (valid) {
      MDisps *owned = MEM_new_array_zeroed<MDisps>(mesh->corners_num, "staged compact layer grids");
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(owned[corner], grids[corner]);
      }
      staged.layers.emplace_back(&layer, owned);
    }
    /* The layer's final grids are owned by staged; discard its duplicate mesh,
     * attributes and paint-mask storage before processing the next layer. */
    BKE_id_free(nullptr, surface);
  }
  valid = valid && multires_force_sculpt_rebuild(object);
  if (valid) {
    for (MultiresSculptLayer *layer : empty) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      for (int corner = 0; corner < layer->grids_num; corner++) {
        MEM_SAFE_DELETE(layer->disps[corner].disps);
      }
      multires_publish_empty_subdivided_layer(*layer, top_level);
    }
    for (const auto &[layer, grids] : staged.layers) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      multires_publish_subdivided_layer(*layer, grids, top_level);
    }
    MDisps *destination = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
    MDisps *new_base = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&base->corner_data, CD_MDISPS, mesh->corners_num));
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      std::swap(destination[corner], new_base[corner]);
    }
    GridPaintMask *masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    GridPaintMask *new_masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &base->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    if (masks && new_masks) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(masks[corner], new_masks[corner]);
      }
    }
    multires_set_tot_level(object, mmd, top_level);
    if (mmd->sculpt_face_set_grids) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    mmd->sculpt_layer_recording = 0;
  }
  if (BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_PROFILE")) {
    std::printf("[SUBDIVIDE_COMPACT] success=%d layers=%d empty=%d active=%d live_layer_meshes_max=1 total=%.3fms\n",
                int(valid), int(mmd->sculpt_layers.count()), int(empty.size()),
                int(staged.layers.size()), (BLI_time_now_seconds() - started) * 1000.0);
  }
  return valid;
}
void multiresModifier_subdivide_to_level(Object *object,
                                         MultiresModifierData *mmd,
                                         const int top_level,
                                         const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }
  Mesh *mesh = id_cast<Mesh *>(object->data);

  /* Capture Face Sets before subdivision. From this point onward the CCG-grid
   * master survives independently of the coarse face-domain attribute. */
  if (mmd->totlvl > 0 && mesh->attributes().contains(".sculpt_face_set")) {
    if (!bke::multires_layers::faceset_ensure_from_mesh(*mmd, *mesh, mmd->totlvl)) {
      BKE_modifier_set_error(object, &mmd->modifier,
                             "Cannot initialize high-resolution Multires Face Sets");
      return;
    }
  }

  if (mmd->totlvl == 0 && !mmd->sculpt_layers.is_empty()) {
    if (!bke::multires_layers::base_remap_after_topology_edit(*mmd, *mesh) ||
        !bke::multires_layers::commit(*object)) {
      BKE_modifier_set_error(object, &mmd->modifier, "Cannot preserve L0 layers during subdivision");
      return;
    }
    multiresModifier_subdivide_to_level_base(object, mmd, top_level, mode);
    if (mmd->totlvl == top_level) {
      bke::multires_layers::initialize_layer_grids(*mmd, top_level, mesh->corners_num);
      bke::multires_layers::ensure_corner_ids(*mmd, *mesh);
    }
    return;
  }
  if (mmd->sculpt_layers.is_empty()) {
    multiresModifier_subdivide_to_level_base(object, mmd, top_level, mode);
    if (mmd->sculpt_face_set_grids && mmd->totlvl >= top_level) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    return;
  }
  if (!bke::multires_layers::remap_after_topology_edit(*mmd, *mesh)) {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Cannot remap Sculpt Layers after the base topology edit");
    return;
  }
  if (top_level > 13 || ID_REAL_USERS(&mesh->id) > 1 ||
      !bke::multires_layers::compatible(*mmd, *mesh) ||
      !bke::multires_layers::commit(*object))
  {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Cannot subdivide: sculpt layer grids or pending edits are invalid");
    return;
  }

  const char *compact_env = BLI_getenv("BLENDER_SCULPT_SUBDIVIDE_COMPACT");
  if (compact_env && compact_env[0] == '1' && compact_env[1] == '\0') {
    if (!multires_subdivide_compact_layers(object, mmd, top_level, mode)) {
      BKE_modifier_set_error(object, &mmd->modifier,
                             "Compact Sculpt Layer subdivision failed; original grids retained");
    }
    return;
  }

  /* V4.5c sparse subdivision safety.
   *
   * Sparse Sculpt Layer grids use nullptr as an implicit all-zero payload.
   * The legacy subdivision pipeline is global, so materialize only existing
   * positive-level layer grids here. A level-zero layer legitimately has no
   * grid payload yet: its base_deltas are carried by the cage and the null
   * high-resolution contribution must remain equivalent to zero. */
  const char *sparse_env = BLI_getenv("BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE");
  const bool keep_empty = sparse_env != nullptr && sparse_env[0] == '1' && sparse_env[1] == '\0';
  std::vector<MultiresSculptLayer *> empty_layers;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (keep_empty && multires_layer_can_keep_empty_subdivide(layer)) {
      empty_layers.push_back(&layer);
      continue;
    }
    if (layer.level <= 0) {
      continue;
    }
    if (!layer.disps || layer.grids_num <= 0) {
      BKE_modifier_set_error(
          object, &mmd->modifier, "Cannot subdivide: invalid sparse Sculpt Layer descriptors");
      return;
    }
    for (int corner = 0; corner < layer.grids_num; corner++) {
      if (bke::multires_layers::grid_ensure(layer, corner) == nullptr) {
        BKE_modifier_set_error(
            object, &mmd->modifier, "Cannot subdivide: failed to materialize Sculpt Layer grid");
        return;
      }
    }
  }

  Mesh *base = multires_subdivide_layer_surface(object, *mmd, nullptr, top_level, mode);
  std::vector<std::pair<MultiresSculptLayer *, Mesh *>> surfaces;
  bool valid = base != nullptr;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (std::find(empty_layers.begin(), empty_layers.end(), &layer) != empty_layers.end()) {
      continue;
    }
    if (!valid) {
      break;
    }
    Mesh *surface = multires_subdivide_layer_surface(object, *mmd, &layer, top_level, mode);
    surfaces.emplace_back(&layer, surface);
    valid = surface != nullptr;
  }
  const MDisps *base_grids = base ? static_cast<const MDisps *>(
      CustomData_get_layer(&base->corner_data, CD_MDISPS)) : nullptr;
  for (const auto &[layer, surface] : surfaces) {
    if (!valid) {
      break;
    }
    MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &surface->corner_data, CD_MDISPS, surface->corners_num));
    valid = multires_stage_subdivided_layer(
        *layer, base_grids, grids, mesh->corners_num, top_level);
  }
  valid = valid && multires_force_sculpt_rebuild(object);
  if (valid) {
    for (MultiresSculptLayer *layer : empty_layers) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      multires_publish_empty_subdivided_layer(*layer, top_level);
    }
    if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      std::printf("[LAYER_EMPTY_SUBDIVIDE] skipped=%d grids=%d old_level=%d new_level=%d\n",
                  int(empty_layers.size()), mesh->corners_num, mmd->totlvl, top_level);
    }
    for (const auto &[layer, surface] : surfaces) {
      MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
          &surface->corner_data, CD_MDISPS, surface->corners_num));
      multires_publish_subdivided_layer(*layer, grids, top_level);
    }
    MDisps *destination = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
    MDisps *new_base = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&base->corner_data, CD_MDISPS, mesh->corners_num));
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      std::swap(destination[corner], new_base[corner]);
    }
    GridPaintMask *masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    GridPaintMask *new_masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &base->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    if (masks && new_masks) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(masks[corner], new_masks[corner]);
      }
    }
    multires_set_tot_level(object, mmd, top_level);
    if (mmd->sculpt_face_set_grids) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    mmd->sculpt_layer_recording = 0;
  }
  else {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Sculpt Layer subdivision failed; original grids retained");
  }
  if (base) {
    BKE_id_free(nullptr, base);
  }
  for (const auto &[layer, surface] : surfaces) {
    if (surface) {
      BKE_id_free(nullptr, surface);
    }
  }
}
void old_subdivide(Object *object,
                                         MultiresModifierData *mmd,
                                         const int top_level,
                                         const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }
  Mesh *mesh = id_cast<Mesh *>(object->data);

  /* Capture Face Sets before subdivision. From this point onward the CCG-grid
   * master survives independently of the coarse face-domain attribute. */
  if (mmd->totlvl > 0 && mesh->attributes().contains(".sculpt_face_set")) {
    if (!bke::multires_layers::faceset_ensure_from_mesh(*mmd, *mesh, mmd->totlvl)) {
      BKE_modifier_set_error(object, &mmd->modifier,
                             "Cannot initialize high-resolution Multires Face Sets");
      return;
    }
  }

  if (mmd->totlvl == 0 && !mmd->sculpt_layers.is_empty()) {
    if (!bke::multires_layers::base_remap_after_topology_edit(*mmd, *mesh) ||
        !bke::multires_layers::commit(*object)) {
      BKE_modifier_set_error(object, &mmd->modifier, "Cannot preserve L0 layers during subdivision");
      return;
    }
    multiresModifier_subdivide_to_level_base(object, mmd, top_level, mode);
    if (mmd->totlvl == top_level) {
      bke::multires_layers::initialize_layer_grids(*mmd, top_level, mesh->corners_num);
      bke::multires_layers::ensure_corner_ids(*mmd, *mesh);
    }
    return;
  }
  if (mmd->sculpt_layers.is_empty()) {
    multiresModifier_subdivide_to_level_base(object, mmd, top_level, mode);
    if (mmd->sculpt_face_set_grids && mmd->totlvl >= top_level) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    return;
  }
  if (!bke::multires_layers::remap_after_topology_edit(*mmd, *mesh)) {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Cannot remap Sculpt Layers after the base topology edit");
    return;
  }
  if (top_level > 13 || ID_REAL_USERS(&mesh->id) > 1 ||
      !bke::multires_layers::compatible(*mmd, *mesh) ||
      !bke::multires_layers::commit(*object))
  {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Cannot subdivide: sculpt layer grids or pending edits are invalid");
    return;
  }

  /* V4.5c sparse subdivision safety.
   *
   * Sparse Sculpt Layer grids use nullptr as an implicit all-zero payload.
   * The legacy subdivision pipeline is global, so materialize only existing
   * positive-level layer grids here. A level-zero layer legitimately has no
   * grid payload yet: its base_deltas are carried by the cage and the null
   * high-resolution contribution must remain equivalent to zero. */
  const char *sparse_env = BLI_getenv("BLENDER_SCULPT_EMPTY_LAYER_SUBDIVIDE");
  const bool keep_empty = sparse_env != nullptr && sparse_env[0] == '1' && sparse_env[1] == '\0';
  std::vector<MultiresSculptLayer *> empty_layers;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (keep_empty && multires_layer_can_keep_empty_subdivide(layer)) {
      empty_layers.push_back(&layer);
      continue;
    }
    if (layer.level <= 0) {
      continue;
    }
    if (!layer.disps || layer.grids_num <= 0) {
      BKE_modifier_set_error(
          object, &mmd->modifier, "Cannot subdivide: invalid sparse Sculpt Layer descriptors");
      return;
    }
    for (int corner = 0; corner < layer.grids_num; corner++) {
      if (bke::multires_layers::grid_ensure(layer, corner) == nullptr) {
        BKE_modifier_set_error(
            object, &mmd->modifier, "Cannot subdivide: failed to materialize Sculpt Layer grid");
        return;
      }
    }
  }

  Mesh *base = multires_subdivide_layer_surface(object, *mmd, nullptr, top_level, mode);
  std::vector<std::pair<MultiresSculptLayer *, Mesh *>> surfaces;
  bool valid = base != nullptr;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (std::find(empty_layers.begin(), empty_layers.end(), &layer) != empty_layers.end()) {
      continue;
    }
    if (!valid) {
      break;
    }
    Mesh *surface = multires_subdivide_layer_surface(object, *mmd, &layer, top_level, mode);
    surfaces.emplace_back(&layer, surface);
    valid = surface != nullptr;
  }
  const MDisps *base_grids = base ? static_cast<const MDisps *>(
      CustomData_get_layer(&base->corner_data, CD_MDISPS)) : nullptr;
  for (const auto &[layer, surface] : surfaces) {
    if (!valid) {
      break;
    }
    MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
        &surface->corner_data, CD_MDISPS, surface->corners_num));
    valid = multires_stage_subdivided_layer(
        *layer, base_grids, grids, mesh->corners_num, top_level);
  }
  valid = valid && multires_force_sculpt_rebuild(object);
  if (valid) {
    for (MultiresSculptLayer *layer : empty_layers) {
      bke::multires_layers::weight_hot_delta_cache_invalidate(layer);
      multires_publish_empty_subdivided_layer(*layer, top_level);
    }
    if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
      std::printf("[LAYER_EMPTY_SUBDIVIDE] skipped=%d grids=%d old_level=%d new_level=%d\n",
                  int(empty_layers.size()), mesh->corners_num, mmd->totlvl, top_level);
    }
    for (const auto &[layer, surface] : surfaces) {
      MDisps *grids = static_cast<MDisps *>(CustomData_get_layer_for_write(
          &surface->corner_data, CD_MDISPS, surface->corners_num));
      multires_publish_subdivided_layer(*layer, grids, top_level);
    }
    MDisps *destination = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&mesh->corner_data, CD_MDISPS, mesh->corners_num));
    MDisps *new_base = static_cast<MDisps *>(
        CustomData_get_layer_for_write(&base->corner_data, CD_MDISPS, mesh->corners_num));
    for (int corner = 0; corner < mesh->corners_num; corner++) {
      std::swap(destination[corner], new_base[corner]);
    }
    GridPaintMask *masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &mesh->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    GridPaintMask *new_masks = static_cast<GridPaintMask *>(CustomData_get_layer_for_write(
        &base->corner_data, CD_GRID_PAINT_MASK, mesh->corners_num));
    if (masks && new_masks) {
      for (int corner = 0; corner < mesh->corners_num; corner++) {
        std::swap(masks[corner], new_masks[corner]);
      }
    }
    multires_set_tot_level(object, mmd, top_level);
    if (mmd->sculpt_face_set_grids) {
      bke::multires_layers::faceset_resample_up(*mmd, top_level);
    }
    mmd->sculpt_layer_recording = 0;
  }
  else {
    BKE_modifier_set_error(object, &mmd->modifier,
                           "Sculpt Layer subdivision failed; original grids retained");
  }
  if (base) {
    BKE_id_free(nullptr, base);
  }
  for (const auto &[layer, surface] : surfaces) {
    if (surface) {
      BKE_id_free(nullptr, surface);
    }
  }
}

struct Fixture{Mesh*mesh;Object object;MultiresModifierData m;
 Fixture(int level,int kind):mesh(new Mesh),object{mesh}{m.totlvl=m.lvl=m.sculptlvl=m.renderlvl=level;int side=bke::subdiv::grid_size_from_level(level),area=side*side;mesh->corner_data.grids=MEM_new_array_zeroed<MDisps>(8,"base");mesh->corner_data.masks=MEM_new_array_zeroed<GridPaintMask>(8,"masks");
  for(int c=0;c<8;c++){auto&g=mesh->corner_data.grids[c];g.level=level;g.totdisp=area;g.disps=MEM_new_array_zeroed<float[3]>(area,"base data");for(int i=0;i<area;i++)for(int a=0;a<3;a++)g.disps[i][a]=float(c+i+a)*.012f;mesh->corner_data.masks[c].level=level;mesh->corner_data.masks[c].data=MEM_new_array_zeroed<float>(area,"mask");}
  m.sculpt_layers.resize(4);for(int k=0;k<4;k++){auto&l=m.sculpt_layers[k];l.level=level;l.grids_num=8;l.strength=float(k)*.2f-.3f;l.flag=k&1;l.group_id=8+k;l.name="Layer"+std::to_string(k);l.disps=MEM_new_array_zeroed<MDisps>(8,"layer grids");for(int c=0;c<8;c++){auto&g=l.disps[c];g.level=level;g.totdisp=area;if(k==0||(kind==1&&k==1)||(kind==2&&k==2)||kind==3){g.disps=MEM_new_array_zeroed<float[3]>(area,"layer data");if((k==0&&kind!=5)||kind==3)for(int i=0;i<area;i++)for(int a=0;a<3;a++)g.disps[i][a]=float(i+k+c+a)*.001f;}if(kind==2&&k==2){g.hidden=BLI_BITMAP_NEW(area,"layer hidden");BLI_BITMAP_SET(g.hidden,0,true);}}
   if(kind==4&&k==3){l.base_verts_num=2;l.base_deltas=MEM_new_array_zeroed<float[3]>(2,"L0");l.base_deltas[0][0]=.2f;}
  }
 }
 ~Fixture(){free_mesh(mesh);for(auto&l:m.sculpt_layers){for(int c=0;c<l.grids_num;c++){MEM_delete(l.disps[c].disps);MEM_delete(l.disps[c].hidden);}MEM_delete(l.disps);MEM_delete(l.base_deltas);}}
};
std::vector<float>values(Fixture&f){std::vector<float>v{float(f.m.totlvl),float(f.m.sculpt_layer_recording)};auto grid=[&](MDisps&g){v.push_back(g.level);v.push_back(g.totdisp);for(int i=0;i<g.totdisp;i++){for(int a=0;a<3;a++)v.push_back(g.disps?g.disps[i][a]:0.f);v.push_back(g.hidden?BLI_BITMAP_TEST_BOOL(g.hidden,i):0.f);}};for(int c=0;c<8;c++){grid(f.mesh->corner_data.grids[c]);int side=bke::subdiv::grid_size_from_level(f.mesh->corner_data.masks[c].level);for(int i=0;i<side*side;i++)v.push_back(f.mesh->corner_data.masks[c].data[i]);}for(auto&l:f.m.sculpt_layers){v.push_back(l.level);v.push_back(l.strength);v.push_back(l.flag);v.push_back(l.group_id);if(l.base_deltas)v.push_back(l.base_deltas[0][0]);for(int c=0;c<8;c++)grid(l.disps[c]);}return v;}
std::string exact_snapshot(Fixture&f){std::string s;auto add=[&](const void*p,size_t n){s.append(static_cast<const char*>(p),n);};add(&f.m.totlvl,sizeof(f.m.totlvl));add(&f.m.sculpt_layer_recording,sizeof(f.m.sculpt_layer_recording));for(auto&l:f.m.sculpt_layers){add(&l.level,sizeof(l.level));for(int c=0;c<8;c++){auto&g=l.disps[c];add(&g,sizeof(g));if(g.disps)add(g.disps,sizeof(float[3])*g.totdisp);}}return s;}
void reset(){calls=live_meshes=max_meshes=rebuilds=bke::multires_layers::invalidations=0;fail_call=0;fail_rebuild=false;peak_bytes=bytes;}
int main(){int cases=0;size_t best_saved=0;
 for(int level=2;level<=5;level++)for(int kind=0;kind<=5;kind++)for(auto mode:{MultiresSubdivideModeType::CatmullClark,MultiresSubdivideModeType::Simple,MultiresSubdivideModeType::Linear}){
  std::vector<float>expected;int legacy_calls=0,legacy_meshes=0;size_t old_peak=0;
  {Fixture a(level,kind);reset();opt=false;old_subdivide(&a.object,&a.m,level+1,mode);legacy_calls=calls;legacy_meshes=max_meshes;old_peak=peak_bytes;expected=values(a);}assert(allocations.empty());
  {Fixture b(level,kind);reset();opt=true;multiresModifier_subdivide_to_level(&b.object,&b.m,level+1,mode);
   assert(expected==values(b));assert(b.m.totlvl==level+1&&live_meshes==0&&max_meshes<=2&&calls<=legacy_calls);if(kind==3)assert(legacy_meshes==5&&max_meshes==2);if(kind==5)assert(calls==1);
   if(old_peak>peak_bytes)best_saved=std::max(best_saved,old_peak-peak_bytes);++cases;
  }assert(allocations.empty()&&bytes==0);
 }
 for(int fail=1;fail<=5;fail++){{Fixture f(3,3);reset();opt=true;fail_call=fail;auto before=exact_snapshot(f);multiresModifier_subdivide_to_level(&f.object,&f.m,4,MultiresSubdivideModeType::CatmullClark);assert(f.m.totlvl==3&&before==exact_snapshot(f)&&live_meshes==0&&bke::multires_layers::invalidations==0);++cases;}assert(allocations.empty());}
 {Fixture f(3,3);reset();opt=true;fail_rebuild=true;auto before=exact_snapshot(f);multiresModifier_subdivide_to_level(&f.object,&f.m,4,MultiresSubdivideModeType::CatmullClark);assert(before==exact_snapshot(f)&&live_meshes==0&&bke::multires_layers::invalidations==0);++cases;}
 {Fixture f(3,3);reset();opt=true;f.m.sculpt_layers[3].disps[7].disps[0][0]=NAN;auto before=exact_snapshot(f);multiresModifier_subdivide_to_level(&f.object,&f.m,4,MultiresSubdivideModeType::CatmullClark);assert(before==exact_snapshot(f)&&live_meshes==0);++cases;}
 assert(allocations.empty()&&bytes==0&&best_saved>0);
 std::cout<<"PASS "<<cases<<" actual legacy/compact staging, sparse/allocated zeros, hidden, L0, 3 modes, atomic failure and tracked-allocation cleanup cases; stub geometry backend; measured stub peak saving="<<best_saved<<" bytes\n";
}
