
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
struct float3 {
 union { struct {float x,y,z;};float data[3];};
 float3(float a=0):x(a),y(a),z(a){}
 explicit float3(int a):x(float(a)),y(float(a)),z(float(a)){}
 float3(float a,float b,float c):x(a),y(b),z(c){}
 explicit float3(const float *p):x(p[0]),y(p[1]),z(p[2]){}
 operator float*(){return data;}operator const float*()const{return data;}
 float3 operator+(float3 b)const{return {x+b.x,y+b.y,z+b.z};}
 float3 operator-(float3 b)const{return {x-b.x,y-b.y,z-b.z};}
 float3 operator*(float f)const{return {x*f,y*f,z*f};}
 float3 operator/(float f)const{return {x/f,y/f,z/f};}
 float3&operator*=(float f){x*=f;y*=f;z*=f;return *this;}
};
float3 operator*(float f,float3 a){return a*f;}
namespace math {
float dot(float3 a,float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float3 cross(float3 a,float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float length_squared(float3 a){return dot(a,a);}
float distance_squared(float3 a,float3 b){return length_squared(a-b);}
float length(float3 a){return std::sqrt(length_squared(a));}
float3 normalize(float3 a){float d=length(a);return d?a/d:float3(0);}
}
void copy_v3_v3(float *a,const float *b){std::memcpy(a,b,12);}
float3 closest_edge(float3 p,float3 a,float3 b){float3 d=b-a;float len=math::dot(d,d);return a+d*std::clamp(len?math::dot(p-a,d)/len:0.0f,0.0f,1.0f);}
void closest_on_tri_to_point_v3(float*out,const float*point,const float*av,const float*bv,const float*cv){
 float3 p(point),a(av),b(bv),c(cv),ab=b-a,ac=c-a,n=math::cross(ab,ac);
 float nn=math::dot(n,n);float3 q=nn?p-n*(math::dot(p-a,n)/nn):a;
 float aa=math::dot(ab,ab),bb=math::dot(ac,ac),x=math::dot(ab,ac),den=aa*bb-x*x;
 float u=0,v=0;if(den>1e-20f){u=(bb*math::dot(q-a,ab)-x*math::dot(q-a,ac))/den;v=(aa*math::dot(q-a,ac)-x*math::dot(q-a,ab))/den;}
 if(den<=1e-20f || u<0 || v<0 || u+v>1){
  q=closest_edge(p,a,b);float best=math::distance_squared(p,q);
  for(float3 e:{closest_edge(p,b,c),closest_edge(p,c,a)}){float dd=math::distance_squared(p,e);if(dd<best){q=e;best=dd;}}
 }
 copy_v3_v3(out,q);
}

#include <optional>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>

struct float2{float x,y;float2(float a=0):x(a),y(a){}float2(float a,float b):x(a),y(b){}float2 operator+(float2 b)const{return{x+b.x,y+b.y};}float2 operator-(float2 b)const{return{x-b.x,y-b.y};}};
namespace math{float length_squared(float2 a){return a.x*a.x+a.y*a.y;}float length(float2 a){return std::sqrt(length_squared(a));}float distance(float3 a,float3 b){return length(a-b);}}
struct IndexRange{
 int64_t first,count;IndexRange(int64_t n=0):first(0),count(n){}IndexRange(int64_t a,int64_t n):first(a),count(n){}
 struct It{int64_t i;int64_t operator*()const{return i;}It&operator++(){i++;return*this;}bool operator!=(It b)const{return i!=b.i;}};
 It begin()const{return{first};}It end()const{return{first+count};}int64_t start()const{return first;}int64_t size()const{return count;}
};
template<class T>struct Arr:std::vector<T>{using std::vector<T>::vector;bool is_empty()const{return this->empty();}IndexRange index_range()const{return IndexRange(this->size());}};
template<class T>struct Span{const T*p;int64_t n;Span(const T*d,int64_t size):p(d),n(size){}template<class U>Span(const Span<U>&o):p(o.data()),n(o.size()){}int64_t size()const{return n;}const T*data()const{return p;}const T&operator[](int64_t i)const{return p[i];}const T*begin()const{return p;}const T*end()const{return p+n;}IndexRange index_range()const{return IndexRange(n);}};
struct IndexMaskMemory{};
namespace exec_mode{int grain_size(int x){return x;}}
struct IndexMask{std::vector<int>ids;
 template<class F>void foreach_index(F fn)const{for(int i:ids)fn(i);}auto begin()const{return ids.begin();}auto end()const{return ids.end();}
 static IndexMask from_indices(Span<int>a,IndexMaskMemory&){IndexMask out;out.ids.assign(a.begin(),a.end());return out;}
 template<class R,class F>static IndexMask from_predicate(const R&r,IndexMaskMemory&,F fn,int){IndexMask out;for(auto i:r)if(fn(i))out.ids.push_back(i);return out;}
};
namespace threading{template<class Fn>void parallel_for(IndexRange range,int grain,Fn fn){
 if(range.size()<4){fn(range);return;}std::vector<std::thread>workers;
 for(int t=0;t<4;t++){int64_t a=range.start()+range.size()*t/4,b=range.start()+range.size()*(t+1)/4;workers.emplace_back([=,&fn]{fn(IndexRange(a,b-a));});}for(auto&t:workers)t.join();
}}
template<class T>struct Bounds{T min,max;};
struct Depsgraph{};struct Object;struct SubdivCCG{int grid_size=5,grid_area=25,grids_num=16;Arr<float3>positions,normals;Arr<float>masks;Arr<Arr<bool>>grid_hidden;};
namespace bke{
struct PaintRuntime{float brush_rotation=0,brush_rotation_sec=0,pixel_radius=30,anchored_size=30;float anchored_initial_mouse[2]={};};
namespace pbvh{
struct Node{Bounds<float3>box;const Bounds<float3>&bounds()const{return box;}};
struct GridsNode:Node{Arr<int>grid_ids;Arr<float3>original,original_normals;Span<int>grids()const{return{grid_ids.data(),int64_t(grid_ids.size())};}};
struct Tree{Arr<GridsNode>data;template<class T>Span<T>nodes(){return{data.data(),int64_t(data.size())};}template<class T>Span<const T>nodes()const{return{data.data(),int64_t(data.size())};}void tag_positions_changed(const IndexMask&){}void update_bounds(Depsgraph&,Object&);};
Tree*object_tree(Object&);Tree*unused=nullptr;
template<class Fn>IndexMask search_nodes(Tree&tree,IndexMaskMemory&,Fn fn){IndexMask out;for(int i=0;i<int(tree.data.size());i++)if(fn(tree.data[i]))out.ids.push_back(i);return out;}
IndexMask all_leaf_nodes(Tree&t,IndexMaskMemory&){IndexMask out;for(int i=0;i<int(t.data.size());i++)out.ids.push_back(i);return out;}
void update_normals(Depsgraph&,Object&,Tree&){}
}}
struct Paint{bke::PaintRuntime*runtime;};
struct StrokeCache{float3 location,location_symm,sculpt_normal={0,0,1};float radius=.5f;Paint*paint=nullptr;};
struct SculptSession{SubdivCCG*subdiv_ccg=nullptr;StrokeCache*cache=nullptr;bke::pbvh::Tree*tree=nullptr;};
struct Runtime{SculptSession*sculpt_session;};struct Object{Runtime*runtime;};
struct Sculpt{Paint paint;};struct ToolSettings{Sculpt*sculpt;};struct bContext{Object*object;ToolSettings*settings;Depsgraph*depsgraph;};struct ViewContext{};struct wmEvent{int mval[2];};
Object*CTX_data_active_object(bContext*C){return C->object;}ToolSettings*CTX_data_tool_settings(bContext*C){return C->settings;}Depsgraph*CTX_data_depsgraph_pointer(bContext*C){return C->depsgraph;}
void ED_workspace_status_text(bContext*,const char*){}
double BLI_time_now_seconds(){using namespace std::chrono;return duration<double>(steady_clock::now().time_since_epoch()).count();}
namespace bke{namespace object{pbvh::Tree*pbvh_get(Object&ob){return ob.runtime->sculpt_session->tree;}}}
namespace bke::pbvh{void Tree::update_bounds(Depsgraph&,Object&ob){auto&ccg=*ob.runtime->sculpt_session->subdiv_ccg;for(auto&node:data){node.box.min=float3(1e10f);node.box.max=float3(-1e10f);for(int g:node.grid_ids)for(int j=0;j<ccg.grid_area;j++){auto p=ccg.positions[g*ccg.grid_area+j];node.box.min.x=std::min(node.box.min.x,p.x);node.box.min.y=std::min(node.box.min.y,p.y);node.box.min.z=std::min(node.box.min.z,p.z);node.box.max.x=std::max(node.box.max.x,p.x);node.box.max.y=std::max(node.box.max.y,p.y);node.box.max.z=std::max(node.box.max.z,p.z);}}}}
struct OrigPositionData{Span<float3>positions,normals;};
std::optional<OrigPositionData>orig_position_data_lookup_grids(Object&,const bke::pbvh::GridsNode&node){if(node.original.empty())return std::nullopt;return OrigPositionData{{node.original.data(),int64_t(node.original.size())},{node.original_normals.data(),int64_t(node.original_normals.size())}};}
namespace undo{enum class Type{Position};void push_nodes(Depsgraph&,Object&ob,const IndexMask&mask,Type){auto&ss=*ob.runtime->sculpt_session;mask.foreach_index([&](int i){auto&node=ss.tree->data[i];if(!node.original.empty())return;for(int g:node.grid_ids)for(int j=0;j<ss.subdiv_ccg->grid_area;j++){node.original.push_back(ss.subdiv_ccg->positions[g*ss.subdiv_ccg->grid_area+j]);node.original_normals.push_back(ss.subdiv_ccg->normals[g*ss.subdiv_ccg->grid_area+j]);}});}
void restore_from_undo_step(Depsgraph&,Sculpt&,Object&ob){auto&ss=*ob.runtime->sculpt_session;for(auto&node:ss.tree->data){if(node.original.empty())continue;int k=0;for(int g:node.grid_ids)for(int j=0;j<ss.subdiv_ccg->grid_area;j++)ss.subdiv_ccg->positions[g*ss.subdiv_ccg->grid_area+j]=node.original[k++];}}}
enum class UpdateType{Position};void flush_update_step(ViewContext&,Object&,UpdateType){}
struct CursorGeometryInfo{float3 location,normal;};std::optional<CursorGeometryInfo>cursor_geometry_info_update(bContext*,float2 m,bool){if(std::abs(m.x)>100||std::abs(m.y)>100)return std::nullopt;return CursorGeometryInfo{{m.x*.01f,m.y*.01f,0},{0,0,1}};}
void apply_translations(Span<float3>d,Span<int>grids,SubdivCCG&ccg){for(int k=0;k<grids.size();k++)for(int j=0;j<ccg.grid_area;j++)ccg.positions[grids[k]*ccg.grid_area+j]=ccg.positions[grids[k]*ccg.grid_area+j]+d[k*ccg.grid_area+j];}
struct BVHTree{int size=0;};struct BVHTreeNearest{int index=-1;float dist_sq=0;float co[3]={};};using Callback=void(*)(void*,int,const float*,BVHTreeNearest*);
BVHTree*BLI_bvhtree_new(int size,float,int,int){return new BVHTree{size};}void BLI_bvhtree_free(BVHTree*t){delete t;}void BLI_bvhtree_insert(BVHTree*,int,const float*,int){}void BLI_bvhtree_balance(BVHTree*){}
void BLI_bvhtree_find_nearest(BVHTree*t,const float*co,BVHTreeNearest*n,Callback cb,void*data){for(int i=0;i<t->size;i++)cb(data,i,co,n);}
struct Triangle {
  std::array<float3, 3> co, delta, normal;
};
struct ReferenceSample {
  int index = -1;
  float3 position = float3(0), normal = float3(0);
  float mask_factor = 1.0f;
  bool hidden = false;
};
struct SavedPosition { int index; float3 position; };
struct State {
  std::vector<Triangle> triangles;
  BVHTree *tree = nullptr;
  SubdivCCG *ccg = nullptr;
  float3 center = float3(0), normal = float3(0, 0, 1);
  float radius = 0, support = 0;
  float2 mouse_offset = float2(0);
  float2 target_mouse = float2(0);
  float angle = 0.0f, rotation_base = 0.0f, rotation_mouse_start = 0.0f;
  bool rotating = false, scaling = false, strength_adjust = false;
  float footprint_scale = 1.0f, imprint_strength = 1.0f;
  float scale_base = 1.0f, scale_mouse_start = 1.0f;
  float strength_base = 1.0f, strength_mouse_start = 0.0f;
  struct Pending { int index; float3 delta; };
  std::vector<Pending> pending;
  std::vector<int> write_grids;
  std::vector<float3> write_translations;
  bool adjusting() const { return rotating || scaling || strength_adjust; }
  float initial_brush_rotation = 0.0f, initial_mask_rotation = 0.0f;
  const char *failure_reason = nullptr;
  int64_t capture_samples = 0;
  bool reference_path = false, parallel = true, profile = false, coherent = true;
  bool target_cached = false, region_cached = false, mapping_cached = false;
  float2 cached_mouse = float2(0);
  float3 cached_location = float3(0), cached_normal = float3(0);
  float region_support = 0.0f, mapped_angle = 0.0f, mapped_scale = 1.0f;
  std::vector<int> region_nodes, region_grids, preview_nodes;
  std::vector<ReferenceSample> region_samples;
  std::vector<float3> mapped_deltas;
  std::vector<int> nearest_seeds;
  std::vector<SavedPosition> preview_before;
  int64_t positions_num = 0, profile_frames = 0;
  std::array<double,5> profile_total = {};
  bool placing = false, valid = false;
  ~State() {
    if (profile && profile_frames) {
      std::printf("[DRAW_TRANSPORT_V06_SUMMARY] frames=%lld avg_restore_ms=%.3f avg_surface_ms=%.3f avg_map_ms=%.3f avg_write_ms=%.3f avg_update_ms=%.3f\n",
          static_cast<long long>(profile_frames),profile_total[0]/profile_frames,
          profile_total[1]/profile_frames,profile_total[2]/profile_frames,
          profile_total[3]/profile_frames,profile_total[4]/profile_frames);
      std::fflush(stdout);
    }
    if (tree) { BLI_bvhtree_free(tree); }
  }
};
inline bool finite(const float3 &p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
inline float3 rotate(const float3 &v, const float3 &from, const float3 &to)
{
  const float c = std::clamp(math::dot(from, to), -1.0f, 1.0f);
  if (c > 0.999999f) { return v; }
  if (c < -0.9999f) {
    float3 axis = math::cross(from, float3(1, 0, 0));
    if (math::length_squared(axis) < 1e-8f) { axis = math::cross(from, float3(0, 1, 0)); }
    axis = math::normalize(axis);
    return 2.0f * math::dot(axis, v) * axis - v;
  }
  const float3 axis = math::normalize(math::cross(from, to));
  const float sine = std::sqrt(std::max(0.0f,1.0f-c*c));
  return c*v + sine*math::cross(axis,v) + (1.0f-c)*math::dot(axis,v)*axis;
}
inline float3 rotate_axis(const float3 &v, const float3 &axis, const float angle)
{
  const float cosine = std::cos(angle), sine = std::sin(angle);
  return cosine*v + sine*math::cross(axis,v) + (1.0f-cosine)*math::dot(axis,v)*axis;
}
/* Scale only coordinates tangent to the imprint plane. The normal/depth coordinate stays unchanged. */
inline float3 footprint_inverse(const float3 &relative, const float3 &normal, const float scale)
{
  const float3 depth = normal*math::dot(relative,normal);
  return depth+(relative-depth)/scale;
}
inline float footprint_from_mouse(const float base, const float distance, const float start)
{
  return std::clamp(base*std::max(distance,8.0f)/start,0.05f,8.0f);
}
inline float strength_from_mouse(const float base, const float mouse, const float start)
{
  return std::clamp(base+(mouse-start)*0.01f,0.0f,10.0f);
}
inline void nearest_cb(void *userdata, int index, const float co[3], BVHTreeNearest *nearest)
{
  const State &state = *static_cast<const State *>(userdata);
  const Triangle &tri = state.triangles[index];
  float3 point;
  closest_on_tri_to_point_v3(point, co, tri.co[0], tri.co[1], tri.co[2]);
  const float d2 = math::distance_squared(point, float3(co));
  if (d2 < nearest->dist_sq) {
    nearest->index = index;
    nearest->dist_sq = d2;
    copy_v3_v3(nearest->co, point);
  }
}
inline bool weights(const Triangle &tri, const float3 &p, float3 &w)
{
  const float3 a = tri.co[1]-tri.co[0], b = tri.co[2]-tri.co[0], q = p-tri.co[0];
  const double aa = math::dot(a,a), ab = math::dot(a,b), bb = math::dot(b,b);
  const double aq = math::dot(a,q), bq = math::dot(b,q), det = aa*bb-ab*ab;
  if (det <= 1e-14*std::max(aa,bb)*std::max(aa,bb) || !std::isfinite(det)) { return false; }
  w.y = float((bb*aq-ab*bq)/det); w.z = float((aa*bq-ab*aq)/det); w.x = 1-w.y-w.z;
  return finite(w);
}
inline bool fail(State &state, const char *reason)
{
  state.failure_reason = reason;
  std::fprintf(stderr,"[DRAW_TRANSPORT_V06] stopped=%s source_samples=%lld triangles=%zu\n",
               reason,static_cast<long long>(state.capture_samples),state.triangles.size());
  return false;
}
inline bool capture(State &state, Object &ob)
{
  const double capture_start = BLI_time_now_seconds();
  const char *reference = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_REFERENCE");
  const char *serial = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_SERIAL");
  const char *profiling = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_PROFILE");
  const char *coherence = std::getenv("BLENDER_SCULPT_DRAW_TRANSPORT_NO_COHERENCE");
  state.coherent = !(coherence && coherence[0] == '1');
  state.reference_path = reference && reference[0] == '1';
  state.parallel = !(serial && serial[0] == '1');
  state.profile = profiling && profiling[0] == '1';
  SculptSession &ss = *ob.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || ss.cache->radius <= 1e-8f) { return fail(state,"No active Multires imprint or zero radius"); }
  state.ccg = ss.subdiv_ccg;
  state.positions_num = state.ccg->positions.size();
  state.center = ss.cache->location;
  state.normal = math::normalize(ss.cache->sculpt_normal);
  if (!finite(state.normal) || math::length_squared(state.normal) < .5f) { return fail(state,"Invalid source normal"); }
  state.radius = ss.cache->radius;
  state.initial_brush_rotation = ss.cache->paint->runtime->brush_rotation;
  state.initial_mask_rotation = ss.cache->paint->runtime->brush_rotation_sec;
  const Span<bke::pbvh::GridsNode> nodes = bke::object::pbvh_get(ob)->nodes<bke::pbvh::GridsNode>();
  const int side = state.ccg->grid_size, area = state.ccg->grid_area;
  int64_t samples = 0;
  bool any_delta = false;
  for (const auto &node : nodes) {
    const std::optional<OrigPositionData> original = orig_position_data_lookup_grids(ob, node);
    if (!original) { continue; }
    samples += int64_t(node.grids().size()) * area;
    state.capture_samples = samples;
    if (original->positions.size() != node.grids().size()*area ||
        original->normals.size() != original->positions.size()) { return fail(state,"Undo/source grid layout differs"); }
    if (!state.reference_path) { state.preview_nodes.push_back(int(&node-nodes.data())); }
    int base = 0;
    for (const int grid : node.grids()) {
      if (!state.reference_path) {
        for (int j=0;j<area;j++) {
          const int index=grid*area+j;
          const float3 before=original->positions[base+j];
          const float3 current=state.ccg->positions[index];
          if (before.x!=current.x || before.y!=current.y || before.z!=current.z) {
            state.preview_before.push_back({index,before});
          }
        }
      }
      for (int y = 0; y < side-1; y++) {
        for (int x = 0; x < side-1; x++) {
          const int a = y*side+x, b = a+1, c = a+side, d = c+1;
          for (const std::array<int,3> corners : {std::array<int,3>{a,b,d}, std::array<int,3>{a,d,c}}) {
            Triangle tri;
            bool changed = false;
            for (int j = 0; j < 3; j++) {
              const int local = corners[j];
              tri.co[j] = original->positions[base+local];
              tri.normal[j] = original->normals[base+local];
              tri.delta[j] = state.ccg->positions[grid*area+local]-tri.co[j];
              if (!finite(tri.co[j]) || !finite(tri.delta[j]) || !finite(tri.normal[j])) { return fail(state,"Non-finite imprint coordinates or normals"); }
              changed |= math::length_squared(tri.delta[j]) > 1e-20f;
            }
            if (!changed) { continue; }
            if (math::length_squared(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0])) < 1e-24f) { continue; }
            any_delta = true;
            for (int j=0;j<3;j++) { state.support = std::max(state.support, math::distance(tri.co[j],state.center)); }
            /* BVH primitive IDs are signed int; this is an API representability check, not a prototype sample cap. */
            if (state.triangles.size() >= size_t(std::numeric_limits<int>::max())) {
              return fail(state,"BVH primitive IDs cannot represent this footprint");
            }
            state.triangles.push_back(tri);
          }
        }
      }
      base += area;
    }
  }
  if (!any_delta) { return fail(state,"No nonzero displacement in the Anchored imprint"); }
  state.tree = BLI_bvhtree_new(int(state.triangles.size()), 0.0f, 4, 6);
  if (!state.tree) { return fail(state,"Cannot allocate the source BVH"); }
  for (int i = 0; i < int(state.triangles.size()); i++) {
    BLI_bvhtree_insert(state.tree, i, state.triangles[i].co[0], 3);
  }
  const double tree_start = BLI_time_now_seconds();
  BLI_bvhtree_balance(state.tree);
  if (state.profile) {
    std::printf("[DRAW_TRANSPORT_V06_CAPTURE] total_ms=%.3f balance_ms=%.3f triangles=%zu source_samples=%lld sparse_restore_samples=%zu reference=%d parallel=%d\n",
        (BLI_time_now_seconds()-capture_start)*1000.0,(BLI_time_now_seconds()-tree_start)*1000.0,
        state.triangles.size(),static_cast<long long>(samples),state.preview_before.size(),int(state.reference_path),int(state.parallel));
  }
  state.placing = true;
  state.valid = true; /* Initial imprint remains a valid position until first motion. */
  std::printf("[DRAW_TRANSPORT_V06] captured samples=%lld triangles=%zu radius=%.6f\n",
              static_cast<long long>(samples), state.triangles.size(), state.radius);
  std::fflush(stdout);
  return true;
}
inline bool sphere_bounds(const Bounds<float3> &bounds, const float3 &center, const float radius)
{
  float d2 = 0;
  for (int i=0;i<3;i++) {
    const float d = std::max({bounds.min[i]-center[i], 0.0f, center[i]-bounds.max[i]});
    d2 += d*d;
  }
  return d2 <= radius*radius;
}
/* Native Undo originals stay authoritative. Never incrementally move an already moved imprint. */
inline bool move_reference(State &state, bContext *C, ViewContext &vc, const wmEvent &event)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  if (ss.subdiv_ccg != state.ccg || !ss.cache) { return fail(state,"Multires surface changed during positioning"); }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  undo::restore_from_undo_step(depsgraph, sd, ob);
  pbvh.update_bounds(depsgraph, ob);
  bke::pbvh::update_normals(depsgraph, ob, pbvh);
  state.valid = false;
  const float2 event_mouse(float(event.mval[0]),float(event.mval[1]));
  const float2 mouse = state.adjusting() ? state.target_mouse : event_mouse+state.mouse_offset;
  if (state.rotating) {
    const float2 radial = event_mouse-state.target_mouse;
    if (math::length_squared(radial) >= 4.0f) {
      /* Same polar-angle convention as Anchored; the captured imprint already contains its initial angle. */
      const float polar = std::atan2(radial.y,radial.x);
      state.angle = state.rotation_base+polar-state.rotation_mouse_start;
    }
  }
  if (state.scaling) {
    state.footprint_scale = footprint_from_mouse(state.scale_base,
        math::length(event_mouse-state.target_mouse),state.scale_mouse_start);
  }
  if (state.strength_adjust) {
    state.imprint_strength = strength_from_mouse(state.strength_base,event_mouse.x,state.strength_mouse_start);
  }
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(C, mouse, false);
  if (!hit) { flush_update_step(vc, ob, UpdateType::Position); return true; }
  const float3 normal = math::normalize(hit->normal);
  if (!finite(normal) || math::length_squared(normal) < .5f) { return fail(state,"Invalid destination normal"); }
  const float max_distance = std::max(state.radius*.25f, 1e-5f);
  const float support = state.support*std::max(1.0f,state.footprint_scale)+max_distance;
  IndexMaskMemory memory;
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const IndexMask leaves = bke::pbvh::all_leaf_nodes(pbvh,memory);
  const IndexMask candidates = IndexMask::from_predicate(leaves, memory,
      [&](int i) { return sphere_bounds(nodes[i].bounds(), hit->location, support); }, exec_mode::grain_size(64));
  /* Capture every intersecting grid node before changing its positions, including shared borders. */
  int64_t sample_count = 0;
  candidates.foreach_index([&](int i) { sample_count += int64_t(nodes[i].grids().size())*state.ccg->grid_area; });
  undo::push_nodes(depsgraph, ob, candidates, undo::Type::Position);
  auto &staged = state.pending;
  staged.clear();
  staged.reserve(size_t(sample_count));
  const int area = state.ccg->grid_area;
  candidates.foreach_index([&](int node) {
    for (const int grid : nodes[node].grids()) {
      for (int j=0;j<area;j++) {
        const int index = grid*area+j;
        if (!state.ccg->grid_hidden.is_empty() && state.ccg->grid_hidden[grid][j]) { continue; }
        const float3 p = state.ccg->positions[index];
        if (math::distance_squared(p,hit->location) > support*support ||
            math::dot(state.ccg->normals[index],normal) < .25f) { continue; }
        const float3 unrotated = rotate(rotate_axis(p-hit->location,normal,-state.angle), normal, state.normal);
        const float3 query = state.center+footprint_inverse(unrotated,state.normal,state.footprint_scale);
        BVHTreeNearest nearest{};
        nearest.index = -1; nearest.dist_sq = max_distance*max_distance;
        BLI_bvhtree_find_nearest(state.tree, query, &nearest, nearest_cb, &state);
        if (nearest.index < 0) { continue; }
        const Triangle &tri = state.triangles[nearest.index];
        float3 w;
        if (!weights(tri,float3(nearest.co),w)) { continue; }
        const float3 source_normal = math::normalize(w.x*tri.normal[0]+w.y*tri.normal[1]+w.z*tri.normal[2]);
        if (math::dot(source_normal,state.normal) < .25f) { continue; }
        /* Do not extend the closest nonzero edge into a plateau outside the captured stamp. */
        const float3 separation = query-float3(nearest.co);
        const float3 face_normal = math::normalize(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0]));
        const float3 tangent = separation-face_normal*math::dot(separation,face_normal);
        const float tangent_tolerance = std::max(state.radius*1e-4f,1e-6f);
        if (math::length_squared(tangent) > tangent_tolerance*tangent_tolerance) { continue; }
        const float3 delta = w.x*tri.delta[0]+w.y*tri.delta[1]+w.z*tri.delta[2];
        float3 moved = rotate_axis(rotate(delta,source_normal,normal),normal,state.angle)*state.imprint_strength;
        /* Source attenuation is already captured; apply the destination's mask as well. */
        if (!state.ccg->masks.is_empty()) { moved *= std::clamp(1.0f-state.ccg->masks[index],0.0f,1.0f); }
        if (!finite(moved)) { continue; }
        if (math::length_squared(moved) > 1e-20f) { staged.push_back({index,moved}); }
      }
    }
  });
  if (staged.empty() && state.imprint_strength != 0.0f) { flush_update_step(vc, ob, UpdateType::Position); return true; }
  auto &grids = state.write_grids;
  auto &translations = state.write_translations;
  grids.clear();
  translations.clear();
  grids.reserve(staged.size());
  translations.reserve(size_t(sample_count));
  int previous_grid = -1;
  for (const State::Pending &p : staged) {
    const int grid = p.index/area;
    if (grid != previous_grid) {
      grids.push_back(grid);
      translations.resize(translations.size()+area,float3(0));
      previous_grid = grid;
    }
    translations[translations.size()-area+p.index%area] = p.delta;
  }
  /* Use the established writer: it records exact dirty grids AND samples for layer Commit. */
  if (!grids.empty()) { apply_translations(Span<float3>(translations.data(),translations.size()),
                     Span<int>(grids.data(),grids.size()),*state.ccg); }
  pbvh.tag_positions_changed(candidates);
  pbvh.update_bounds(depsgraph, ob);
  flush_update_step(vc, ob, UpdateType::Position);
  ss.cache->location = hit->location;
  ss.cache->location_symm = hit->location;
  sd.paint.runtime->anchored_initial_mouse[0] = mouse.x;
  sd.paint.runtime->anchored_initial_mouse[1] = mouse.y;
  sd.paint.runtime->brush_rotation = state.initial_brush_rotation+state.angle;
  sd.paint.runtime->brush_rotation_sec = state.initial_mask_rotation+state.angle;
  sd.paint.runtime->anchored_size = sd.paint.runtime->pixel_radius*state.footprint_scale;
  state.target_mouse = mouse;
  state.valid = true;
  char status[320];
  if (state.strength_adjust) {
    std::snprintf(status,sizeof(status),"Draw Transport STRENGTH %.3fx | mouse left/right | Click/Enter: accept | Esc: cancel stroke",state.imprint_strength);
  }
  else if (state.scaling) {
    std::snprintf(status,sizeof(status),"Draw Transport SCALE %.3fx (depth unchanged) | S again: strength | Click/Enter: accept | Esc: cancel stroke",state.footprint_scale);
  }
  else if (state.rotating) {
    std::snprintf(status,sizeof(status),"Draw Transport ROTATE %.1f deg | Click/Enter/R: accept | Esc: cancel stroke",state.angle*57.295779513f);
  }
  else {
    std::snprintf(status,sizeof(status),"Draw Transport MOVE | scale %.3fx strength %.3fx | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc: cancel",state.footprint_scale,state.imprint_strength);
  }
  ED_workspace_status_text(C,status);
  return true;
}
/* Prepared transforms preserve the V04 arithmetic while moving constant trig/frame work out of the sample loop. */
struct PreparedAxisRotation {
  float3 axis;
  float cosine, sine;
  PreparedAxisRotation(const float3 &axis_, const float angle)
      : axis(axis_), cosine(std::cos(angle)), sine(std::sin(angle)) {}
  float3 apply(const float3 &v) const
  {
    return cosine*v+sine*math::cross(axis,v)+(1.0f-cosine)*math::dot(axis,v)*axis;
  }
};
struct PreparedNormalRotation {
  float3 axis = float3(0);
  float cosine = 1.0f, sine = 0.0f;
  int mode = 0;
  PreparedNormalRotation(const float3 &from, const float3 &to)
  {
    cosine=std::clamp(math::dot(from,to),-1.0f,1.0f);
    if (cosine>0.999999f) { return; }
    if (cosine< -0.9999f) {
      mode=1;
      axis=math::cross(from,float3(1,0,0));
      if (math::length_squared(axis)<1e-8f) { axis=math::cross(from,float3(0,1,0)); }
      axis=math::normalize(axis);
      return;
    }
    mode=2;
    axis=math::normalize(math::cross(from,to));
    sine=std::sqrt(std::max(0.0f,1.0f-cosine*cosine));
  }
  float3 apply(const float3 &v) const
  {
    if (!mode) { return v; }
    if (mode==1) { return 2.0f*math::dot(axis,v)*axis-v; }
    return cosine*v+sine*math::cross(axis,v)+(1.0f-cosine)*math::dot(axis,v)*axis;
  }
};
/* Read-only kernel: each worker owns one output slot and its local BVH query. */
inline float3 map_reference_sample(const State &state, const ReferenceSample &sample,
                                   const float3 &location, const float3 &normal,
                                   const float support, const float max_distance,
                                   const PreparedAxisRotation &inverse_angle,
                                   const PreparedNormalRotation &inverse_normal,
                                   const PreparedAxisRotation &output_angle,
                                   int *seed = nullptr, const int neighbor_seed = -1)
{
  if (sample.hidden || math::distance_squared(sample.position,location)>support*support ||
      math::dot(sample.normal,normal)<.25f) { return float3(0); }
  const float3 unrotated=inverse_normal.apply(inverse_angle.apply(sample.position-location));
  const float3 query=state.center+footprint_inverse(unrotated,state.normal,state.footprint_scale);
  BVHTreeNearest nearest{};
  nearest.index=-1; nearest.dist_sq=max_distance*max_distance;
  /* Coherent triangles give a valid upper distance bound, never a replacement
   * for the global nearest search. Do not seed the result index: ties still go
   * through the normal tree traversal. Inflate the bound to include its owner. */
  const float original_limit=nearest.dist_sq;
  if (seed) {
    BVHTreeNearest hint{};
    hint.index=-1;hint.dist_sq=original_limit;
    for (const int candidate : {*seed,neighbor_seed}) {
      if (candidate>=0 && size_t(candidate)<state.triangles.size()) {
        nearest_cb(const_cast<State *>(&state),candidate,query,&hint);
      }
    }
    if (hint.index>=0) { nearest.dist_sq=std::nextafter(hint.dist_sq,original_limit); }
  }
  BLI_bvhtree_find_nearest(state.tree,query,&nearest,nearest_cb,const_cast<State *>(&state));
  if (nearest.index<0 && nearest.dist_sq<original_limit) {
    /* Defensive fallback for floating-point boundary rejection. */
    nearest.dist_sq=original_limit;
    BLI_bvhtree_find_nearest(state.tree,query,&nearest,nearest_cb,const_cast<State *>(&state));
  }
  if (seed) { *seed=nearest.index; }
  if (nearest.index<0) { return float3(0); }
  const Triangle &tri=state.triangles[nearest.index];
  float3 w;
  if (!weights(tri,float3(nearest.co),w)) { return float3(0); }
  const float3 source_normal=math::normalize(w.x*tri.normal[0]+w.y*tri.normal[1]+w.z*tri.normal[2]);
  if (math::dot(source_normal,state.normal)<.25f) { return float3(0); }
  const float3 separation=query-float3(nearest.co);
  const float3 face_normal=math::normalize(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0]));
  const float3 tangent=separation-face_normal*math::dot(separation,face_normal);
  const float tolerance=std::max(state.radius*1e-4f,1e-6f);
  if (math::length_squared(tangent)>tolerance*tolerance) { return float3(0); }
  const float3 delta=w.x*tri.delta[0]+w.y*tri.delta[1]+w.z*tri.delta[2];
  return output_angle.apply(rotate(delta,source_normal,normal));
}
inline float3 weighted_delta(const float3 &mapped, const float strength, const float mask_factor)
{
  /* Keep the original order: imprint Strength first, destination mask second. */
  return (mapped*strength)*mask_factor;
}
inline bool effective_delta(const float3 &delta)
{
  return finite(delta) && math::length_squared(delta)>1e-20f;
}
inline IndexMask index_mask(const std::vector<int> &indices, IndexMaskMemory &memory)
{
  return IndexMask::from_indices(Span<int>(indices.data(),indices.size()),memory);
}
inline int64_t restore_preview(State &state, bke::pbvh::Tree &pbvh)
{
  const int64_t count=state.preview_before.size();
  /* Exact assignments avoid subtraction drift over repeated moves/SS adjustments. */
  for (const SavedPosition &saved:state.preview_before) {
    state.ccg->positions[saved.index]=saved.position;
  }
  if (!state.preview_nodes.empty()) {
    IndexMaskMemory memory;
    pbvh.tag_positions_changed(index_mask(state.preview_nodes,memory));
  }
  state.preview_before.clear();
  state.preview_nodes.clear();
  return count;
}
inline void profile_frame(State &state, const std::array<double,5> &times,
                            const int64_t restored, const bool reused_region,
                            const bool reused_mapping)
{
  if (!state.profile) { return; }
  state.profile_frames++;
  for (int i=0;i<5;i++) { state.profile_total[i]+=times[i]; }
  if (state.profile_frames<=8 || state.profile_frames%15==0) {
    std::printf("[DRAW_TRANSPORT_V06_FRAME] frame=%lld mode=%s restored=%lld samples=%zu reuse_region=%d reuse_mapping=%d parallel=%d restore_ms=%.3f surface_ms=%.3f map_ms=%.3f write_ms=%.3f update_ms=%.3f total_ms=%.3f\n",
        static_cast<long long>(state.profile_frames),state.strength_adjust?"SS":state.scaling?"S":state.rotating?"R":"MOVE",
        static_cast<long long>(restored),state.region_samples.size(),int(reused_region),int(reused_mapping),int(state.parallel),
        times[0],times[1],times[2],times[3],times[4],times[0]+times[1]+times[2]+times[3]+times[4]);
    std::fflush(stdout);
  }
}
inline void update_status(State &state,bContext *C)
{
  char status[320];
  if (state.strength_adjust) {
    std::snprintf(status,sizeof(status),"Draw Transport STRENGTH %.3fx | mouse left/right | Click/Enter: accept | Esc: cancel stroke",state.imprint_strength);
  }
  else if (state.scaling) {
    std::snprintf(status,sizeof(status),"Draw Transport SCALE %.3fx (depth unchanged) | S again: strength | Click/Enter: accept | Esc: cancel stroke",state.footprint_scale);
  }
  else if (state.rotating) {
    std::snprintf(status,sizeof(status),"Draw Transport ROTATE %.1f deg | Click/Enter/R: accept | Esc: cancel stroke",state.angle*57.295779513f);
  }
  else {
    std::snprintf(status,sizeof(status),"Draw Transport MOVE | scale %.3fx strength %.3fx | R: rotate | S: scale / SS: strength | Click/Enter: confirm | Esc: cancel",state.footprint_scale,state.imprint_strength);
  }
  ED_workspace_status_text(C,status);
}
inline bool move_optimized(State &state,bContext *C,ViewContext &vc,const wmEvent &event)
{
  Object &ob=*CTX_data_active_object(C);
  SculptSession &ss=*ob.runtime->sculpt_session;
  Sculpt &sd=*CTX_data_tool_settings(C)->sculpt;
  Depsgraph &depsgraph=*CTX_data_depsgraph_pointer(C);
  if (ss.subdiv_ccg!=state.ccg || !ss.cache || state.ccg->positions.size()!=state.positions_num) {
    return fail(state,"Multires surface changed during positioning");
  }
  std::array<double,5> times={};
  double phase=BLI_time_now_seconds();
  bke::pbvh::Tree &pbvh=*bke::object::pbvh_get(ob);
  const int64_t restored=restore_preview(state,pbvh);
  times[0]=(BLI_time_now_seconds()-phase)*1000.0; phase=BLI_time_now_seconds();
  state.valid=false;
  const float2 event_mouse(float(event.mval[0]),float(event.mval[1]));
  const float2 mouse=state.adjusting()?state.target_mouse:event_mouse+state.mouse_offset;
  if (state.rotating) {
    const float2 radial=event_mouse-state.target_mouse;
    if (math::length_squared(radial)>=4.0f) {
      state.angle=state.rotation_base+std::atan2(radial.y,radial.x)-state.rotation_mouse_start;
    }
  }
  if (state.scaling) {
    state.footprint_scale=footprint_from_mouse(state.scale_base,math::length(event_mouse-state.target_mouse),state.scale_mouse_start);
  }
  if (state.strength_adjust) {
    state.imprint_strength=strength_from_mouse(state.strength_base,event_mouse.x,state.strength_mouse_start);
  }
  const bool same_target=state.target_cached && mouse.x==state.cached_mouse.x && mouse.y==state.cached_mouse.y;
  bool baseline_updated=false;
  if (!same_target) {
    /* Current geometry is pristine after sparse restore, before raycast and reference capture. */
    pbvh.update_bounds(depsgraph,ob);
    bke::pbvh::update_normals(depsgraph,ob,pbvh);
    baseline_updated=true;
    const std::optional<CursorGeometryInfo> hit=cursor_geometry_info_update(C,mouse,false);
    state.target_cached=state.region_cached=state.mapping_cached=false;
    if (!hit) {
      flush_update_step(vc,ob,UpdateType::Position);
      times[1]=(BLI_time_now_seconds()-phase)*1000.0;
      profile_frame(state,times,restored,false,false);
      return true;
    }
    state.cached_location=hit->location;
    state.cached_normal=math::normalize(hit->normal);
    if (!finite(state.cached_normal) || math::length_squared(state.cached_normal)<.5f) {
      return fail(state,"Invalid destination normal");
    }
    state.cached_mouse=mouse;
    state.target_cached=true;
  }
  const float3 location=state.cached_location,normal=state.cached_normal;
  const float max_distance=std::max(state.radius*.25f,1e-5f);
  const float support=state.support*std::max(1.0f,state.footprint_scale)+max_distance;
  const bool reused_region=state.region_cached && same_target && support<=state.region_support;
  const int area=state.ccg->grid_area;
  if (!reused_region) {
    if (!baseline_updated) {
      pbvh.update_bounds(depsgraph,ob);
      bke::pbvh::update_normals(depsgraph,ob,pbvh);
    }
    IndexMaskMemory memory;
    /* Hierarchical PBVH traversal replaces a scan of every leaf node. */
    const IndexMask candidates=bke::pbvh::search_nodes(pbvh,memory,[&](const bke::pbvh::Node &node) {
      return sphere_bounds(node.bounds(),location,support);
    });
    undo::push_nodes(depsgraph,ob,candidates,undo::Type::Position);
    state.region_nodes.clear();state.region_grids.clear();
    const Span<bke::pbvh::GridsNode> nodes=pbvh.nodes<bke::pbvh::GridsNode>();
    candidates.foreach_index([&](int i) {
      state.region_nodes.push_back(i);
      for (const int grid:nodes[i].grids()) { state.region_grids.push_back(grid); }
    });
    state.region_samples.resize(state.region_grids.size()*size_t(area));
    const auto capture_region=[&](const IndexRange range) {
      for (const int64_t offset:range) {
        const int grid=state.region_grids[offset/area],local=int(offset%area),index=grid*area+local;
        ReferenceSample sample;
        sample.index=index;sample.position=state.ccg->positions[index];sample.normal=state.ccg->normals[index];
        sample.mask_factor=state.ccg->masks.is_empty()?1.0f:std::clamp(1.0f-state.ccg->masks[index],0.0f,1.0f);
        sample.hidden=!state.ccg->grid_hidden.is_empty() && state.ccg->grid_hidden[grid][local];
        state.region_samples[offset]=sample;
      }
    };
    if (state.parallel) { threading::parallel_for(IndexRange(state.region_samples.size()),4096,capture_region); }
    else { capture_region(IndexRange(state.region_samples.size())); }
    state.region_support=support;
    state.region_cached=true;state.mapping_cached=false;
    state.nearest_seeds.assign(state.region_samples.size(),-1);
  }
  times[1]=(BLI_time_now_seconds()-phase)*1000.0;phase=BLI_time_now_seconds();
  const bool reused_mapping=state.mapping_cached && reused_region &&
      state.angle==state.mapped_angle && state.footprint_scale==state.mapped_scale;
  if (!reused_mapping) {
    state.mapped_deltas.resize(state.region_samples.size());
    const PreparedAxisRotation inverse_angle(normal,-state.angle),output_angle(normal,state.angle);
    const PreparedNormalRotation inverse_normal(normal,state.normal);
    const auto map_range=[&](const IndexRange range) {
      int neighbor_seed=-1;
      for (const int64_t i:range) {
        state.mapped_deltas[i]=map_reference_sample(state,state.region_samples[i],location,normal,
            support,max_distance,inverse_angle,inverse_normal,output_angle,
            state.coherent ? &state.nearest_seeds[i] : nullptr,neighbor_seed);
        neighbor_seed=state.nearest_seeds[i];
      }
    };
    if (state.parallel) { threading::parallel_for(IndexRange(state.region_samples.size()),1024,map_range); }
    else { map_range(IndexRange(state.region_samples.size())); }
    state.mapped_angle=state.angle;state.mapped_scale=state.footprint_scale;state.mapping_cached=true;
  }
  times[2]=(BLI_time_now_seconds()-phase)*1000.0;phase=BLI_time_now_seconds();
  auto &grids=state.write_grids;auto &translations=state.write_translations;
  grids.clear();translations.clear();
  state.preview_before.reserve(state.region_samples.size());
  grids.reserve(state.region_grids.size());translations.reserve(state.region_samples.size());
  /* Build all backups and write buffers before any mutation; failures use native stroke cancellation. */
  for (size_t g=0;g<state.region_grids.size();g++) {
    const size_t start=g*area;
    bool grid_written=false;
    for (int j=0;j<area;j++) {
      const size_t i=start+j;
      const ReferenceSample &sample=state.region_samples[i];
      const float3 delta=weighted_delta(state.mapped_deltas[i],state.imprint_strength,sample.mask_factor);
      if (!effective_delta(delta)) { continue; }
      if (!grid_written) {
        grids.push_back(state.region_grids[g]);
        translations.resize(translations.size()+area,float3(0));
        grid_written=true;
      }
      translations[translations.size()-area+j]=delta;
      state.preview_before.push_back({sample.index,sample.position});
    }
  }
  if (!grids.empty()) {
    state.preview_nodes=state.region_nodes;
    const auto write_range=[&](const IndexRange range) {
      apply_translations(Span<float3>(translations.data()+range.start()*area,range.size()*area),
                         Span<int>(grids.data()+range.start(),range.size()),*state.ccg);
    };
    if (state.parallel) { threading::parallel_for(IndexRange(grids.size()),std::max(1,4096/area),write_range); }
    else { write_range(IndexRange(grids.size())); }
  }
  times[3]=(BLI_time_now_seconds()-phase)*1000.0;phase=BLI_time_now_seconds();
  if (!state.preview_nodes.empty()) {
    IndexMaskMemory memory;pbvh.tag_positions_changed(index_mask(state.preview_nodes,memory));
  }
  pbvh.update_bounds(depsgraph,ob);
  flush_update_step(vc,ob,UpdateType::Position);
  state.valid=!grids.empty() || state.imprint_strength==0.0f;
  if (state.valid) {
    ss.cache->location=location;ss.cache->location_symm=location;
    sd.paint.runtime->anchored_initial_mouse[0]=mouse.x;sd.paint.runtime->anchored_initial_mouse[1]=mouse.y;
    sd.paint.runtime->brush_rotation=state.initial_brush_rotation+state.angle;
    sd.paint.runtime->brush_rotation_sec=state.initial_mask_rotation+state.angle;
    sd.paint.runtime->anchored_size=sd.paint.runtime->pixel_radius*state.footprint_scale;
    state.target_mouse=mouse;update_status(state,C);
  }
  times[4]=(BLI_time_now_seconds()-phase)*1000.0;
  profile_frame(state,times,restored,reused_region,reused_mapping);
  return true;
}
inline bool move(State &state,bContext *C,ViewContext &vc,const wmEvent &event)
{
  if (state.reference_path) {
    const double start=BLI_time_now_seconds();
    const bool result=move_reference(state,C,vc,event);
    if (state.profile) {
      std::printf("[DRAW_TRANSPORT_V06_REFERENCE] mode=%s total_ms=%.3f\n",
                  state.strength_adjust?"SS":state.scaling?"S":state.rotating?"R":"MOVE",
                  (BLI_time_now_seconds()-start)*1000.0);
    }
    return result;
  }
  return move_optimized(state,C,vc,event);
}

struct Engine{
 SubdivCCG ccg;bke::pbvh::Tree tree;StrokeCache cache;SculptSession ss;Runtime runtime;Object ob;Sculpt sd;ToolSettings settings;Depsgraph depsgraph;bContext C;ViewContext vc;bke::PaintRuntime paint;
 std::vector<float3>baseline;
 Engine(){sd.paint.runtime=&paint;cache.paint=&sd.paint;cache.location={-.5f,-.5f,0};cache.radius=.4f;ss.subdiv_ccg=&ccg;ss.cache=&cache;ss.tree=&tree;runtime.sculpt_session=&ss;ob.runtime=&runtime;settings.sculpt=&sd;C={&ob,&settings,&depsgraph};
 for(int y=0;y<4;y++)for(int x=0;x<4;x++){
  int g=y*4+x;bke::pbvh::GridsNode node;node.grid_ids.push_back(g);tree.data.push_back(node);
  for(int j=0;j<5;j++)for(int i=0;i<5;i++){float xx=-1+x*.5f+i*.125f,yy=-1+y*.5f+j*.125f;ccg.positions.push_back({xx,yy,0});ccg.normals.push_back({0,0,1});ccg.masks.push_back((xx>.75f)?.65f:0);}
 }
 baseline.assign(ccg.positions.begin(),ccg.positions.end());
 IndexMask mask;for(int g=0;g<16;g++)mask.ids.push_back(g);undo::push_nodes(depsgraph,ob,mask,undo::Type::Position);
 for(auto&p:ccg.positions){float x=p.x+.5f,y=p.y+.5f;if(x*x+y*y<.16f)p.z=.07f*std::exp(-16*(x*x+y*y))*(.6f+std::sin(20*x));}
 tree.update_bounds(depsgraph,ob);
 }
};
int main(){
 Engine reference,serial,parallel;State a,b,c;
 assert(capture(a,reference.ob));assert(capture(b,serial.ob));assert(capture(c,parallel.ob));b.parallel=false;c.parallel=true;
 a.target_mouse=b.target_mouse=c.target_mouse={-50,-50};
 size_t checks=0,cache_hits=0;float worst=0;
 for(int frame=0;frame<90;frame++){
  wmEvent event{};int block=frame/15,step=frame%15;
  for(State*s:{&a,&b,&c}){s->rotating=s->scaling=s->strength_adjust=false;
   if(block==0){event.mval[0]=-60+step*8;event.mval[1]=-40+step*5;}
   if(block==1){s->rotating=true;s->rotation_base=.2f;s->rotation_mouse_start=0;event.mval[0]=int(s->target_mouse.x+30);event.mval[1]=int(s->target_mouse.y-30+step*4);}
   if(block==2){s->scaling=true;s->scale_base=1;s->scale_mouse_start=30;event.mval[0]=int(s->target_mouse.x+12+step*6);event.mval[1]=int(s->target_mouse.y);}
   if(block==3){s->strength_adjust=true;s->strength_base=1;s->strength_mouse_start=50;event.mval[0]=-60+step*14;event.mval[1]=20;}
   if(block==4){event.mval[0]=step<5?150:-80+step*6;event.mval[1]=10;}
   if(block==5){s->scaling=true;s->scale_base=1;s->scale_mouse_start=80;event.mval[0]=int(s->target_mouse.x+80-step*4);event.mval[1]=int(s->target_mouse.y);}
  }
  assert(move_reference(a,&reference.C,reference.vc,event));assert(move_optimized(b,&serial.C,serial.vc,event));assert(move_optimized(c,&parallel.C,parallel.vc,event));
  assert(a.valid==b.valid&&b.valid==c.valid);
  for(size_t i=0;i<reference.ccg.positions.size();i++){
   float error=math::distance_squared(reference.ccg.positions[i],serial.ccg.positions[i]);worst=std::max(worst,error);assert(error<1e-12f);
   assert(std::memcmp(&serial.ccg.positions[i],&parallel.ccg.positions[i],sizeof(float3))==0);checks++;
  }
  if(block==3&&step>0){assert(b.mapping_cached&&c.mapping_cached);cache_hits++;}
 }
 // The original native Undo route must still have the before-state of EVERY visited node.
 undo::restore_from_undo_step(reference.depsgraph,reference.sd,reference.ob);
 undo::restore_from_undo_step(serial.depsgraph,serial.sd,serial.ob);
 undo::restore_from_undo_step(parallel.depsgraph,parallel.sd,parallel.ob);
 for(Engine*e:{&reference,&serial,&parallel})assert(std::memcmp(e->ccg.positions.data(),e->baseline.data(),e->baseline.size()*sizeof(float3))==0);
 std::cout<<"PASS: actual capture + V04/V05 move functions over 90 MOVE/R/S/SS/off-mesh/grow/shrink frames; "<<checks<<" position comparisons; serial/parallel bitwise match; native-Undo adapter cancellation exact; max squared error="<<worst<<"; SS reuse checks="<<cache_hits<<"\n";
}
