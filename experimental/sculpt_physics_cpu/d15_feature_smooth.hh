/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace sculpt_physics::feature_smooth_d15 {
using V = std::array<double, 3>;
inline V add(V a,V b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V sub(V a,V b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V mul(V a,double s){return {a[0]*s,a[1]*s,a[2]*s};}
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline double length(V a){return std::hypot(a[0],a[1],a[2]);}
inline bool finite(V a){return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]);}
// Normal-only material transport preserves corner attributes exactly.
// Fail closed for invalid input, without modifying the output.
inline bool normal_component(V motion,V normal,V &out){
 const double n=length(normal);
 if(!finite(motion)||!finite(normal)||!std::isfinite(n)||n<=0.)return false;
 const V unit=mul(normal,1./n);
 const V trial=mul(unit,dot(motion,unit));
 if(!finite(trial))return false;
 out=trial;return true;
}
enum Feature : unsigned { Sharp=1, Seam=2, FaceSet=4, Open=8, UVChart=16, Guide=32 };
struct Vertex { V co{},normal{}; double weight=0.; bool visible=true; };
struct Edge { std::size_t a,b; unsigned flags=0; bool readable=true; };
struct Settings { double normal_keep=.20, max_step=.25, bend_cos=-.5; bool adaptive_boundaries=false; };
struct Result {
 std::vector<V> motion;
 std::size_t interior=0,sliding=0,pinned=0;
};
// CPU oracle only. Immutable snapshot; caller supplies complete incident edges
// for every writable vertex and the read-only halo. Feature masks live on
// edges, not on unrelated neighboring vertices. No Blender writes here.
inline bool evaluate(const std::vector<Vertex>& v,std::vector<Edge> edges,
                     const Settings& k,Result& result){
 if(!std::isfinite(k.normal_keep)||k.normal_keep<0.||k.normal_keep>1.||
    !std::isfinite(k.max_step)||k.max_step<0.||k.max_step>.5||
    !std::isfinite(k.bend_cos)||k.bend_cos< -1.||k.bend_cos>0.)return false;
 for(const auto& x:v)if(!finite(x.co)||!finite(x.normal)||
     !std::isfinite(x.weight)||x.weight<0.||x.weight>1.)return false;
 for(auto& e:edges){
  if(e.a>=v.size()||e.b>=v.size()||e.a==e.b||(e.flags&~63u))return false;
  if(e.a>e.b)std::swap(e.a,e.b);
 }
 std::sort(edges.begin(),edges.end(),[](const Edge& a,const Edge& b){
  return a.a<b.a||(a.a==b.a&&a.b<b.b);
 });
 struct Row {
  V sum{}; std::array<V,2> feature{}; std::array<unsigned,2> flags{};
  std::size_t count=0,degree=0; bool blocked=false,hard=false;
  double shortest=std::numeric_limits<double>::infinity();
 };
 std::vector<Row> rows(v.size());
 for(std::size_t j=0;j<edges.size();++j){
  const auto& e=edges[j];
  if(j&&e.a==edges[j-1].a&&e.b==edges[j-1].b)return false;
  const V d=sub(v[e.b].co,v[e.a].co);
  const double len=length(d);
  if(!finite(d)||!std::isfinite(len))return false;
  const bool readable=e.readable&&v[e.a].visible&&v[e.b].visible&&len>0.;
  const auto visit=[&](std::size_t i,V offset){
   auto& r=rows[i];
   if(e.flags){
    r.hard=r.hard||bool(e.flags&(Sharp|Open|Guide));
    if(r.degree<2){r.feature[r.degree]=offset;r.flags[r.degree]=e.flags;}
    ++r.degree;
    if(!readable)r.blocked=true;
   }
   if(readable){r.sum=add(r.sum,offset);++r.count;r.shortest=std::min(r.shortest,len);}
  };
  visit(e.a,d);visit(e.b,mul(d,-1.));
 }
 Result trial;trial.motion.resize(v.size());
 for(std::size_t i=0;i<v.size();++i){
  const auto& x=v[i];const auto& r=rows[i];
  if(!finite(r.sum))return false;
  if(!x.visible||x.weight==0.)continue;
  V d{};
  if(r.degree && k.adaptive_boundaries && !r.hard){
   // Soft boundaries follow the surface normally, including endpoints
   // and junctions. Only a coherent two-edge chain can slide tangentially.
   const double n=length(x.normal);
   if(r.blocked||!r.count||!std::isfinite(n)||n==0.){++trial.pinned;continue;}
   const V unit=mul(x.normal,1./n);
   const V mean=mul(r.sum,1./double(r.count));
   d=mul(unit,k.normal_keep*dot(mean,unit));
   if(r.degree==2&&r.flags[0]==r.flags[1]){
    const double a=length(r.feature[0]),b=length(r.feature[1]);
    if(a>0.&&b>0.&&dot(mul(r.feature[0],1./a),mul(r.feature[1],1./b))<=k.bend_cos){
     V chord=sub(r.feature[1],r.feature[0]);
     chord=sub(chord,mul(unit,dot(chord,unit)));
     const double c=length(chord);
     if(std::isfinite(c)&&c>0.){
      const V tangent=mul(chord,1./c);
      d=add(d,mul(tangent,dot(mul(add(r.feature[0],r.feature[1]),.5),tangent)));
      ++trial.sliding;
     }
    }
   }
  }else if(r.degree){
   // Conservative V1 policy: pin ends, mixed features, junctions and sharp
   // bends. Only slide along the chord of the actual two feature edges.
   if(r.blocked||r.degree!=2||r.flags[0]!=r.flags[1]){++trial.pinned;continue;}
   const double a=length(r.feature[0]),b=length(r.feature[1]);
   const V u=mul(r.feature[0],1./a),w=mul(r.feature[1],1./b);
   const V chord=sub(r.feature[1],r.feature[0]);const double c=length(chord);
   if(!finite(u)||!finite(w)||!std::isfinite(c)||c==0.||dot(u,w)>k.bend_cos){++trial.pinned;continue;}
   const V tangent=mul(chord,1./c);
   d=mul(tangent,dot(mul(add(r.feature[0],r.feature[1]),.5),tangent));
   ++trial.sliding;
  }else{
   const double n=length(x.normal);
   if(!r.count||!std::isfinite(n)||n==0.){++trial.pinned;continue;}
   const V unit=mul(x.normal,1./n);
   const V mean=mul(r.sum,1./double(r.count));
   d=sub(mean,mul(unit,(1.-k.normal_keep)*dot(mean,unit)));
   ++trial.interior;
  }
  if(!finite(d))return false;
  d=mul(d,x.weight);
  const double l=length(d),cap=k.max_step*r.shortest*x.weight;
  if(!std::isfinite(l)||!std::isfinite(cap))return false;
  if(l>cap&&l>0.)d=mul(d,cap/l);
  if(!finite(d)||!finite(add(x.co,d)))return false;
  trial.motion[i]=d;
 }
 result=std::move(trial);return true;
}
} // namespace sculpt_physics::feature_smooth_d15
