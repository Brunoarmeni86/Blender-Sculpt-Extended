
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
struct Triangle{std::array<float3,3> co,delta,normal;};
struct ReferenceSample{int index=-1;float3 position,normal;float mask_factor=1;bool hidden=false;};
struct State;
struct BVHTree{int size=0;};
struct BVHTreeNearest{int index=-1;float dist_sq=0;float co[3]={};};
using Callback=void(*)(void*,int,const float*,BVHTreeNearest*);
void BLI_bvhtree_find_nearest(BVHTree*,const float*,BVHTreeNearest*,Callback,void*);
struct QueryStats { int64_t queries=0, misses=0, fallbacks=0, culled=0, hinted=0; };
struct State{float3 source_min,source_max;bool source_cull=true,source_bounds_valid=false;std::vector<Triangle> triangles;BVHTree*tree=nullptr;float3 center,normal={0,0,1};float radius=.7f,angle=0,footprint_scale=1,imprint_strength=1;};
void BLI_bvhtree_find_nearest(BVHTree*tree,const float*co,BVHTreeNearest*n,Callback cb,void*data){
 const State&s=*static_cast<State*>(data);
 for(int i=0;i<tree->size;i++){
  float bound=0;
  for(int axis=0;axis<3;axis++){
   float lo=std::min({s.triangles[i].co[0].data[axis],s.triangles[i].co[1].data[axis],s.triangles[i].co[2].data[axis]});
   float hi=std::max({s.triangles[i].co[0].data[axis],s.triangles[i].co[1].data[axis],s.triangles[i].co[2].data[axis]});
   float d=std::max({lo-co[axis],0.0f,co[axis]-hi});bound+=d*d;
  }
  if(bound<=n->dist_sq)cb(data,i,co,n);
 }
}
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
                                   int *seed = nullptr, const int neighbor_seed = -1, QueryStats *stats = nullptr)
{
  if (sample.hidden || sample.mask_factor==0.0f || math::distance_squared(sample.position,location)>support*support ||
      math::dot(sample.normal,normal)<.25f) { return float3(0); }
  const float3 unrotated=inverse_normal.apply(inverse_angle.apply(sample.position-location));
  const float3 query=state.center+footprint_inverse(unrotated,state.normal,state.footprint_scale);
  /* Only reject outside the source AABB expanded by the full query radius.
   * Padding covers transform/float rounding; no projected surface assumption. */
  if (state.source_cull && state.source_bounds_valid) {
    const float padding=max_distance+std::max(state.radius*1e-4f,1e-6f);
    for (int axis=0;axis<3;axis++) {
      if (query[axis]<state.source_min[axis]-padding || query[axis]>state.source_max[axis]+padding) {
        if (stats) { stats->culled++; } return float3(0);
      }
    }
  }
  if (stats) { stats->queries++; }
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
    if (hint.index>=0) { nearest.dist_sq=std::nextafter(hint.dist_sq,original_limit); if (stats) { stats->hinted++; } }
  }
  BLI_bvhtree_find_nearest(state.tree,query,&nearest,nearest_cb,const_cast<State *>(&state));
  if (nearest.index<0 && nearest.dist_sq<original_limit) {
    /* Defensive fallback for floating-point boundary rejection. */
    if (stats) { stats->fallbacks++; }
    nearest.dist_sq=original_limit;
    BLI_bvhtree_find_nearest(state.tree,query,&nearest,nearest_cb,const_cast<State *>(&state));
  }
  if (seed) { *seed=nearest.index; }
  if (stats && nearest.index<0) { stats->misses++; }
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
float3 reference_v04(const State &state,const ReferenceSample &sample,const float3 &location,const float3 &normal,float support,float max_distance){
        if (sample.hidden) { return float3(0); }
        const float3 p = sample.position;
        if (math::distance_squared(p,location) > support*support ||
            math::dot(sample.normal,normal) < .25f) { return float3(0); }
        const float3 unrotated = rotate(rotate_axis(p-location,normal,-state.angle), normal, state.normal);
        const float3 query = state.center+footprint_inverse(unrotated,state.normal,state.footprint_scale);
        BVHTreeNearest nearest{};
        nearest.index = -1; nearest.dist_sq = max_distance*max_distance;
        BLI_bvhtree_find_nearest(state.tree, query, &nearest, nearest_cb, const_cast<State *>(&state));
        if (nearest.index < 0) { return float3(0); }
        const Triangle &tri = state.triangles[nearest.index];
        float3 w;
        if (!weights(tri,float3(nearest.co),w)) { return float3(0); }
        const float3 source_normal = math::normalize(w.x*tri.normal[0]+w.y*tri.normal[1]+w.z*tri.normal[2]);
        if (math::dot(source_normal,state.normal) < .25f) { return float3(0); }
        /* Do not extend the closest nonzero edge into a plateau outside the captured stamp. */
        const float3 separation = query-float3(nearest.co);
        const float3 face_normal = math::normalize(math::cross(tri.co[1]-tri.co[0],tri.co[2]-tri.co[0]));
        const float3 tangent = separation-face_normal*math::dot(separation,face_normal);
        const float tangent_tolerance = std::max(state.radius*1e-4f,1e-6f);
        if (math::length_squared(tangent) > tangent_tolerance*tangent_tolerance) { return float3(0); }
        const float3 delta = w.x*tri.delta[0]+w.y*tri.delta[1]+w.z*tri.delta[2];
        float3 moved = rotate_axis(rotate(delta,source_normal,normal),normal,state.angle)*state.imprint_strength;
        /* Source attenuation is already captured; apply the destination's mask as well. */
        moved *= sample.mask_factor;
        if (!finite(moved)) { return float3(0); }
        if (math::length_squared(moved)>1e-20f) { return moved; }
return float3(0);
}

int main(){
 std::mt19937 rng(71);std::uniform_real_distribution<float>d(-1,1);
 // Prepared rotations must match the actual original expressions, including nearly opposite normals.
 for(int i=0;i<100000;i++){
  float3 a=math::normalize({d(rng),d(rng),d(rng)}),b=math::normalize({d(rng),d(rng),d(rng)}),v={d(rng),d(rng),d(rng)};float angle=d(rng)*6.28f;
  float3 old=rotate(v,a,b),now=PreparedNormalRotation(a,b).apply(v);
  assert(math::distance_squared(old,now)<1e-12f);
  old=rotate_axis(v,a,angle);now=PreparedAxisRotation(a,angle).apply(v);
  assert(math::distance_squared(old,now)<1e-12f);
 }
 size_t checks=0;float max_error=0;
 for(int curved=0;curved<2;curved++){
  State state;BVHTree tree;state.tree=&tree;
  auto vertex=[&](float x,float y,int corner){
   float z=curved?.12f*std::sin(2*x)*std::cos(3*y):0;
   float3 n=math::normalize({curved?-.24f*std::cos(2*x)*std::cos(3*y):0,curved?.36f*std::sin(2*x)*std::sin(3*y):0,1});
   float relief=.025f*std::exp(-5*(x*x+y*y))*(.4f+std::sin(11*x)*std::cos(9*y));
   // Include tiny deltas that become significant only when Strength increases.
   if(corner%13==0)relief*=1e-8f;
   return std::array<float3,3>{float3(x,y,z),n*relief,n};
  };
  for(int y=0;y<8;y++)for(int x=0;x<8;x++){
   float xx=-1+x*.25f,yy=-1+y*.25f;
   std::array<std::array<float3,3>,4> p={vertex(xx,yy,x+y),vertex(xx+.25f,yy,x+y+1),vertex(xx,yy+.25f,x+y+2),vertex(xx+.25f,yy+.25f,x+y+3)};
   for(auto ids:{std::array<int,3>{0,1,3},std::array<int,3>{0,3,2}}){Triangle t;for(int j=0;j<3;j++){t.co[j]=p[ids[j]][0];t.delta[j]=p[ids[j]][1];t.normal[j]=p[ids[j]][2];}state.triangles.push_back(t);}
  }
  tree.size=state.triangles.size();state.source_min={-1,-1,-.13f};state.source_max={1,1,.13f};state.source_bounds_valid=true;
  {
   ReferenceSample outside;outside.position={2,0,0};outside.normal={0,0,1};
   PreparedAxisRotation zero({0,0,1},0);PreparedNormalRotation identity({0,0,1},{0,0,1});
   QueryStats stats;int seed=-100;
   auto result=map_reference_sample(state,outside,float3(0),float3(0,0,1),4,.175f,zero,identity,zero,&seed,-200,&stats);
   assert(math::length_squared(result)==0 && stats.culled==1 && stats.queries==0);
   state.source_cull=false;stats={};
   result=map_reference_sample(state,outside,float3(0),float3(0,0,1),4,.175f,zero,identity,zero,&seed,-200,&stats);
   assert(math::length_squared(result)==0 && stats.queries==1 && stats.misses==1);
   state.source_cull=true;
  }
  for(int pose=0;pose<6;pose++){
   state.angle=d(rng)*3.0f;state.footprint_scale=.15f+(d(rng)+1)*1.9f;
   const float3 normal=math::normalize({d(rng)*.5f,d(rng)*.5f,1}),location={2,1,.6f};
   float support=3.5f,max_distance=.175f;
   std::vector<ReferenceSample> samples(1200);std::vector<float3> mapped(samples.size()),parallel(samples.size());
   for(size_t i=0;i<samples.size();i++){
    float x=d(rng)*1.2f,y=d(rng)*1.2f,z=curved?.12f*std::sin(2*x)*std::cos(3*y):0;
    float3 source={x*state.footprint_scale,y*state.footprint_scale,z};
    samples[i].position=location+rotate(rotate_axis(source,state.normal,state.angle),state.normal,normal);
    samples[i].normal=i%31==0?normal*-1:normal;
    samples[i].mask_factor=i%17==0?0:(.5f+.5f*d(rng));samples[i].hidden=i%29==0;
   }
   PreparedAxisRotation inverse_angle(normal,-state.angle),output_angle(normal,state.angle);PreparedNormalRotation inverse_normal(normal,state.normal);
   auto kernel=[&](size_t begin,size_t end,std::vector<float3>&out){int seed=-1;for(size_t i=begin;i<end;i++)out[i]=map_reference_sample(state,samples[i],location,normal,support,max_distance,inverse_angle,inverse_normal,output_angle,&seed,seed);};
   kernel(0,samples.size(),mapped);
   std::vector<std::thread> workers;
   for(int worker=0;worker<4;worker++)workers.emplace_back([&,worker]{kernel(samples.size()*worker/4,samples.size()*(worker+1)/4,parallel);});
   for(auto&t:workers)t.join();
   assert(std::memcmp(mapped.data(),parallel.data(),mapped.size()*sizeof(float3))==0);
   // Reuse the SAME mapping while SS changes, as the native cache does.
   for(float strength:{0.f,.001f,.4f,1.f,4.f,10.f}){
    state.imprint_strength=strength;
    for(size_t i=0;i<samples.size();i++){
     float3 expected=reference_v04(state,samples[i],location,normal,support,max_distance);
     float3 actual=weighted_delta(mapped[i],strength,samples[i].mask_factor);if(!effective_delta(actual))actual=float3(0);
     float error=math::distance_squared(expected,actual);max_error=std::max(max_error,error);
     assert(error<1e-12f);checks++;
    }
   }
  }
 }
 std::cout<<"PASS: 100000 prepared-transform comparisons; "<<checks<<" V04 vs V05 weighted kernel comparisons on flat/curved patches; SS cache incl zero; four-thread deterministic output; max squared error="<<max_error<<"\n";
}
