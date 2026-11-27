/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace sculpt_physics::surface_anchor_d15 {
using V = std::array<double,3>;
using UV = std::array<double,2>;
using Id = std::uint64_t;
inline V sub(V a,V b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V add(V a,V b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V mul(V a,double s){return {a[0]*s,a[1]*s,a[2]*s};}
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline double norm(V a){return std::hypot(a[0],a[1],a[2]);}
template<std::size_t N> inline bool finite(const std::array<double,N>& a){for(double x:a)if(!std::isfinite(x))return false;return true;}
struct Triangle {
 Id id=0,component=0,chart=0;
 std::array<Id,3> vertices{};
 std::array<V,3> co{};
 std::array<UV,3> uv{};
};
struct Anchor {Id generation=0,triangle=0,component=0,chart=0; V bary{1.,0.,0.};};
struct Sample {V position{};UV uv{};};
struct Query {V position{},normal{};double max_distance=0.,min_normal_dot=.5,ambiguity_distance=1e-8,uv_epsilon=1e-6;unsigned max_hops=2;};
inline bool bary_valid(V b){return finite(b)&&b[0]>=0.&&b[1]>=0.&&b[2]>=0.&&std::abs(b[0]+b[1]+b[2]-1.)<=1e-9;}
inline bool triangle_normal(const Triangle& t,V& n){
 for(int i=0;i<3;++i)if(!finite(t.co[i])||!finite(t.uv[i])||t.vertices[i]==t.vertices[(i+1)%3])return false;
 V a=sub(t.co[1],t.co[0]),b=sub(t.co[2],t.co[0]);double s=std::max(norm(a),norm(b));
 if(!(s>0.)||!std::isfinite(s))return false;
 n=cross(mul(a,1./s),mul(b,1./s));double l=norm(n);
 if(!(l>1e-12)||!std::isfinite(l))return false;
 n=mul(n,1./l);return finite(n);
}
inline bool sample(const Triangle& t,V w,Sample& out){
 if(!bary_valid(w))return false;
 Sample s;
 for(int i=0;i<3;++i){s.position=add(s.position,mul(t.co[i],w[i]));for(int j=0;j<2;++j)s.uv[j]+=t.uv[i][j]*w[i];}
 if(!finite(s.position)||!finite(s.uv))return false;
 out=s;return true;
}
inline bool evaluate(const Triangle& t,Id generation,const Anchor& a,Sample& out){
 V n;
 if(a.generation!=generation||a.triangle!=t.id||a.component!=t.component||a.chart!=t.chart||!triangle_normal(t,n))return false;
 return sample(t,a.bary,out);
}
// Closest point: projected interior if valid, otherwise minimum of three
// closed edge segments. Local normalization protects the Gram determinant.
inline bool closest(const Triangle& t,V p,V& bary,double& distance){
 V n;if(!finite(p)||!triangle_normal(t,n))return false;
 V e=sub(t.co[1],t.co[0]),f=sub(t.co[2],t.co[0]),q=sub(p,t.co[0]);
 double s=std::max(norm(e),norm(f));e=mul(e,1./s);f=mul(f,1./s);q=mul(q,1./s);
 if(!finite(q))return false;
 double best=std::numeric_limits<double>::infinity();V wbest{};
 auto consider=[&](V w){Sample x;if(!sample(t,w,x))return;double d=norm(sub(x.position,p));if(d<best){best=d;wbest=w;}};
 V cr=cross(e,f);double den=dot(cr,cr);
 double v=dot(cross(q,f),cr)/den,w=dot(cross(e,q),cr)/den;
 if(v>=0.&&w>=0.&&v+w<=1.)consider({1.-v-w,v,w});
 for(int i=0;i<3;++i){int j=(i+1)%3;V d=mul(sub(t.co[j],t.co[i]),1./s),r=mul(sub(p,t.co[i]),1./s);double d2=dot(d,d);
  if(!(d2>0.)||!finite(r))continue;
  double u=std::clamp(dot(r,d)/d2,0.,1.);V b{};b[i]=1.-u;b[j]=u;consider(b);
 }
 if(!std::isfinite(best))return false;
 bary=wbest;distance=best;return true;
}
inline bool same_uv(UV a,UV b,double eps){return std::abs(a[0]-b[0])<=eps&&std::abs(a[1]-b[1])<=eps;}
// Caller supplies a bounded local patch and explicit adjacency. Traversal
// requires exactly two identical vertex IDs and matching corner UVs/positions.
inline bool linked(const Triangle& a,const Triangle& b,double eps){
 if(a.component!=b.component||a.chart!=b.chart)return false;
 int count=0;
 for(int i=0;i<3;++i)for(int j=0;j<3;++j)if(a.vertices[i]==b.vertices[j]){
  ++count;if(a.co[i]!=b.co[j]||!same_uv(a.uv[i],b.uv[j],eps))return false;
 }
 return count==2;
}
struct Link {std::size_t a,b;};
enum class Status { Ok,Invalid,NoMatch,Ambiguous };
inline Status relocate(const std::vector<Triangle>& patch,const std::vector<Link>& links,Id generation,
                       const Anchor& seed,const Query& q,Anchor& out){
 if(patch.empty()||patch.size()>4096||links.size()>16384||q.max_hops>32||!finite(q.position)||!finite(q.normal)||
    !std::isfinite(q.max_distance)||q.max_distance<0.||!std::isfinite(q.min_normal_dot)||q.min_normal_dot<0.||q.min_normal_dot>1.||
    !std::isfinite(q.ambiguity_distance)||q.ambiguity_distance<0.||!std::isfinite(q.uv_epsilon)||q.uv_epsilon<0.)return Status::Invalid;
 double nl=norm(q.normal);if(!(nl>0.)||!std::isfinite(nl))return Status::Invalid;
 V normal=mul(q.normal,1./nl);if(!finite(normal))return Status::Invalid;
 std::vector<Id> ids;std::vector<V> normals(patch.size());std::size_t root=patch.size();
 for(std::size_t i=0;i<patch.size();++i){ids.push_back(patch[i].id);if(!triangle_normal(patch[i],normals[i]))return Status::Invalid;if(patch[i].id==seed.triangle)root=i;}
 std::sort(ids.begin(),ids.end());if(std::adjacent_find(ids.begin(),ids.end())!=ids.end()||root==patch.size())return Status::Invalid;
 Sample old;if(!evaluate(patch[root],generation,seed,old))return Status::Invalid;
 std::vector<std::vector<std::size_t>> adj(patch.size());
 for(auto l:links){if(l.a>=patch.size()||l.b>=patch.size()||l.a==l.b)return Status::Invalid;
  if(linked(patch[l.a],patch[l.b],q.uv_epsilon)){adj[l.a].push_back(l.b);adj[l.b].push_back(l.a);}}
 if(dot(normals[root],normal)<q.min_normal_dot)return Status::NoMatch;
 std::vector<unsigned> depth(patch.size(),q.max_hops+1);std::vector<std::size_t> queue{root};depth[root]=0;
 for(std::size_t k=0;k<queue.size();++k){auto i=queue[k];if(depth[i]==q.max_hops)continue;
  for(auto j:adj[i])if(depth[j]>q.max_hops&&dot(normals[j],normal)>=q.min_normal_dot){depth[j]=depth[i]+1;queue.push_back(j);}}
 struct Candidate {std::size_t i;V bary;double distance;Sample s;};std::vector<Candidate> candidates;
 for(auto i:queue){V b;double d;if(!closest(patch[i],q.position,b,d))return Status::Invalid;if(d>q.max_distance)continue;Sample s;if(!sample(patch[i],b,s))return Status::Invalid;candidates.push_back({i,b,d,s});}
 if(candidates.empty())return Status::NoMatch;
 std::sort(candidates.begin(),candidates.end(),[&](const Candidate& a,const Candidate& b){return a.distance!=b.distance?a.distance<b.distance:patch[a.i].id<patch[b.i].id;});
 const auto& best=candidates.front();
 for(const auto& c:candidates){if(c.distance-best.distance>q.ambiguity_distance)break;
  if(norm(sub(c.s.position,best.s.position))>q.ambiguity_distance||!same_uv(c.s.uv,best.s.uv,q.uv_epsilon))return Status::Ambiguous;}
 out={generation,patch[best.i].id,seed.component,seed.chart,best.bary};return Status::Ok;
}
} // namespace sculpt_physics::surface_anchor_d15
