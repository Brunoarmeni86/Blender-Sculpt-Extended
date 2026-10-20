#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include "multires_layer_base_math.hh"
using namespace blender::bke::multires_layers;
using float3=std::array<float,3>;
template<class T> using Array=std::vector<T>;
template<class T> struct Vector:std::vector<T>{void append(T v){this->push_back(v);}bool is_empty(){return this->empty();}};
constexpr int MULTIRES_SCULPT_LAYER_VISIBLE=1,MULTIRES_SCULPT_LAYER_GROUP_HIDDEN=2,CD_MDISPS=3;
struct MDisps{float (*disps)[3]=nullptr;int totdisp=1;};
struct MultiresSculptLayer{int flag=1,group_flag=0,base_verts_num=0,grids_num=1;float strength=1;float (*base_deltas)[3]=nullptr;MDisps *disps=nullptr;};
struct Layers:std::vector<MultiresSculptLayer>{bool is_empty(){return empty();}};
struct MultiresModifierData{Layers sculpt_layers;int totlvl=1,sculpt_layer_active=0,sculpt_layer_recording=1;};
struct ID{void *lib=nullptr;int users=1;};
struct Corner{MDisps *grids=nullptr;bool external=false;};
struct Mesh{ID id;void *key=nullptr;Corner corner_data;int corners_num=1;bool valid=true;std::vector<float3> p{{1,2,3}};auto vert_positions(){return p;}struct Writer{std::vector<float3>&p;void copy_from(const std::vector<float3>&q){p=q;}};Writer vert_positions_for_write(){return{p};}void tag_positions_changed(){}};
#define ID_REAL_USERS(p) ((p)->users)
#define MEM_SAFE_DELETE(p) do{delete[] (p);(p)=nullptr;}while(0)
bool compatible(const MultiresModifierData&,const Mesh&m){return m.valid;}
bool CustomData_external_test(Corner*c,int){return c->external;}
const void *CustomData_get_layer(Corner*c,int){return c->grids;}
void *CustomData_get_layer_for_write(Corner*c,int,int){return c->grids;}
void weight_hot_delta_cache_invalidate(){}
bool is_effective(const MultiresSculptLayer&l){return(l.flag&1)&&std::isfinite(l.strength)&&std::abs(l.strength)>=1e-6f;}
const MultiresSculptLayer *active(MultiresModifierData&m){return &m.sculpt_layers[m.sculpt_layer_active];}
int BLI_findindex(Layers*l,const MultiresSculptLayer*p){return int(p-l->data());}
void remove(MultiresModifierData&,MultiresSculptLayer&){assert(false&&"remove not exercised");}
const char *merge_visible_to_base(MultiresModifierData &mmd, Mesh &mesh,
                                  const bool remove_layers, const bool validate_only)
{
  /* MERGE_VISIBLE_NATIVE_BASE_V1. Keep the original cage/tangent frames.
   * All validations precede CustomData writable access and layer removal.
   * Once published, no further operation in this transaction can fail. */
  if (mesh.id.lib || ID_REAL_USERS(&mesh.id) > 1 || mesh.key ||
      CustomData_external_test(&mesh.corner_data, CD_MDISPS))
  {
    return "Merge requires a single-user mesh without shape keys or external displacement";
  }
  if (mmd.totlvl <= 0 || !compatible(mmd, mesh)) {
    return "Merge requires valid native and layer grids at the maximum Multires level";
  }
  Vector<MultiresSculptLayer *> visible;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
        (layer.group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN)) {
      continue;
    }
    visible.append(&layer);
    if (!is_effective(layer) || !layer.base_deltas) {
      continue;
    }
    for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
      for (int axis = 0; axis < 3; axis++) {
        if (layer.base_deltas[vertex][axis] != 0.0f) {
          return "Visible L0 cage edits require frame rebasing; original layers retained";
        }
      }
    }
  }
  if (visible.is_empty()) {
    return "No visible Sculpt Layers to merge";
  }
  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh.corner_data, CD_MDISPS));
  /* Match native compositor order, zero-strength threshold and sparse reads.
   * Deterministic two-pass computation needs no additional high-resolution
   * staging buffer. A shared CustomData buffer is detached only on publication. */
  for (int corner = 0; corner < mesh.corners_num; corner++) {
    for (int sample = 0; sample < base[corner].totdisp; sample++) {
      for (int axis = 0; axis < 3; axis++) {
        float value = base[corner].disps[sample][axis];
        if (!std::isfinite(value)) {
          return "Non-finite native displacement; original layers retained";
        }
        for (const MultiresSculptLayer *layer : visible) {
          const MDisps &grid = layer->disps[corner];
          if (is_effective(*layer) && grid.disps) {
            value += grid.disps[sample][axis] * layer->strength;
            if (!std::isfinite(value)) {
              return "Non-finite merged displacement; original layers retained";
            }
          }
        }
      }
    }
  }

  if (validate_only) {
    return nullptr;
  }

  /* Shared by both Bake entry points; invalidate once after numerical preflight. */
  weight_hot_delta_cache_invalidate();
  MDisps *destination = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh.corner_data, CD_MDISPS, mesh.corners_num));
  if (!destination) {
    return "Native displacement is not writable; original layers retained";
  }
  weight_hot_delta_cache_invalidate();
  for (int corner = 0; corner < mesh.corners_num; corner++) {
    for (int sample = 0; sample < destination[corner].totdisp; sample++) {
      for (int axis = 0; axis < 3; axis++) {
        float value = destination[corner].disps[sample][axis];
        for (const MultiresSculptLayer *layer : visible) {
          const MDisps &grid = layer->disps[corner];
          if (is_effective(*layer) && grid.disps) {
            value += grid.disps[sample][axis] * layer->strength;
          }
        }
        destination[corner].disps[sample][axis] = value;
      }
    }
  }
  const MultiresSculptLayer *selected = active(mmd);
  const bool keep_selected = selected &&
      (!(selected->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
       (selected->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN));
  for (MultiresSculptLayer *layer : visible) {
    if (remove_layers) {
      remove(mmd, *layer);
    }
    else {
      for (int corner = 0; corner < layer->grids_num; corner++) {
        MEM_SAFE_DELETE(layer->disps[corner].disps);
      }
      MEM_SAFE_DELETE(layer->base_deltas);
      layer->base_verts_num = 0;
    }
  }
  if (remove_layers && keep_selected) {
    mmd.sculpt_layer_active = BLI_findindex(&mmd.sculpt_layers, selected);
  }
  mmd.sculpt_layer_recording = 0;
  return nullptr;
}

bool bake_visible_to_base(MultiresModifierData &mmd, Mesh &mesh)
{
  if (!compatible(mmd, mesh) || mesh.id.lib || ID_REAL_USERS(&mesh.id) > 1 || mesh.key) {
    return false;
  }
  if (mmd.totlvl > 0) {
    /* BAKE_SPARSE_B04_V01: same-cage HR bake shares the sparse/finite preflight
     * and rejects effective L0 edits that would need tangent-frame rebasing. */
    return merge_visible_to_base(mmd, mesh, false, false) == nullptr;
  }
  /* BAKE_VISIBLE_HIDDEN_V01: hidden offsets remain independent of the baked
   * base. At L0 there are no tangent frames to rebase. Validate and stage only
   * visible contributions, then clear only those same layers. */
  std::vector<base_math::LayerView> visible;
  bool has_visible = false;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
        (layer.group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN))
    {
      continue;
    }
    has_visible = true;
    if ((!layer.base_deltas && layer.base_verts_num == 0) || !is_effective(layer)) {
      continue;
    }
    visible.push_back({layer.base_deltas, layer.base_verts_num, layer.strength, true});
  }
  if (!has_visible) {
    return false;
  }
  Array<float3> baked_positions(mesh.vert_positions());
  if (!base_math::compose(reinterpret_cast<const float (*)[3]>(baked_positions.data()),
                          int(baked_positions.size()),
                          visible,
                          reinterpret_cast<float (*)[3]>(baked_positions.data())))
  {
    return false;
  }
  weight_hot_delta_cache_invalidate();
  mesh.vert_positions_for_write().copy_from(baked_positions);
  mesh.tag_positions_changed();
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
        (layer.group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN))
    {
      continue;
    }
    MEM_SAFE_DELETE(layer.base_deltas);
    layer.base_verts_num = 0;
  }
  mmd.sculpt_layer_recording = 0;
  return true;
}

float (*delta(float x))[3]{auto d=new float[1][3]{};d[0][0]=x;return d;}
struct Fixture{MultiresModifierData m;Mesh mesh;MDisps base;Fixture(bool hr){m.totlvl=hr?1:0;mesh.corner_data.grids=&base;base.disps=delta(1);m.sculpt_layers.resize(3);for(int i=0;i<3;i++){auto&l=m.sculpt_layers[i];l.disps=new MDisps[1];l.disps[0].disps=delta(i+2);l.base_deltas=hr?nullptr:delta(i+2);l.base_verts_num=hr?0:1;}m.sculpt_layers[0].strength=.5;m.sculpt_layers[1].flag=0;m.sculpt_layers[1].strength=-.4;m.sculpt_layers[2].group_flag=2;m.sculpt_layer_active=1;}~Fixture(){delete[]base.disps;for(auto&l:m.sculpt_layers){delete[]l.base_deltas;delete[]l.disps[0].disps;delete[]l.disps;}}};
int main(){
 for(bool hr:{false,true}){
  {Fixture f(hr);auto*h=f.m.sculpt_layers[1].base_deltas;auto*g=f.m.sculpt_layers[2].base_deltas;auto*hd=f.m.sculpt_layers[1].disps[0].disps;assert(bake_visible_to_base(f.m,f.mesh));assert((hr?f.base.disps[0][0]:f.mesh.p[0][0])==2);assert(f.m.sculpt_layers[1].base_deltas==h&&f.m.sculpt_layers[2].base_deltas==g);assert(f.m.sculpt_layers[1].disps[0].disps==hd&&hd[0][0]==3);assert(f.m.sculpt_layer_active==1);assert(bake_visible_to_base(f.m,f.mesh));assert((hr?f.base.disps[0][0]:f.mesh.p[0][0])==2);f.m.sculpt_layers[0].flag=0;assert(!bake_visible_to_base(f.m,f.mesh));}
  {Fixture f(hr);auto&l=f.m.sculpt_layers[0];(hr?l.disps[0].disps:l.base_deltas)[0][0]=NAN;assert(!bake_visible_to_base(f.m,f.mesh));assert((hr?f.base.disps[0][0]:f.mesh.p[0][0])==1);assert((hr?l.disps[0].disps:l.base_deltas)!=nullptr);}
  {Fixture f(hr);auto&h=f.m.sculpt_layers[1];(hr?h.disps[0].disps:h.base_deltas)[0][0]=NAN;assert(bake_visible_to_base(f.m,f.mesh));assert(std::isnan((hr?h.disps[0].disps:h.base_deltas)[0][0]));}
  {Fixture f(hr);f.mesh.valid=false;assert(!bake_visible_to_base(f.m,f.mesh));}
  {Fixture f(hr);f.mesh.id.users=2;assert(!bake_visible_to_base(f.m,f.mesh));}
 }
 {Fixture f(true);f.m.sculpt_layers[0].base_deltas=delta(1);f.m.sculpt_layers[0].base_verts_num=1;assert(!bake_visible_to_base(f.m,f.mesh));assert(f.base.disps[0][0]==1);}
 std::cout<<"PASS actual bake/merge bodies: HR/L0, hidden and hidden-group preservation, all-hidden cancellation, repeat bake, invalid/nonfinite rejection, hidden nonfinite isolation, HR L0-rebase guard\n";
}
