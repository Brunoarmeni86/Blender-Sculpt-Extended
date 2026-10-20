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
