/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "brush_contact.hh"
#include <algorithm>
#include <queue>
#include <limits>
#include <stdexcept>
#include <functional>
#include <map>
namespace sculpt_physics {
namespace hybrid_v31d11 {
using Tri=std::array<std::size_t,3>;
inline double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline double norm(Vec3 a){return std::hypot(a.x,a.y,a.z);}
inline Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline Vec3 unit(Vec3 a){double n=norm(a);return n>1e-20?a/n:Vec3{};}
struct Patch {
 std::vector<Vec3> p,normal;
 std::vector<double> weight,area;
 std::vector<Tri> tris;
 std::vector<std::vector<std::size_t>> adj;
};
inline Patch make_patch(const std::vector<Vec3>& p,const std::vector<Tri>& tris,
 const std::vector<double>& native){
 if(native.size()!=p.size())throw std::invalid_argument("native size");
 Patch s; s.p=p;s.tris=tris;s.weight=native;s.normal.resize(p.size());
 s.area.resize(p.size());s.adj.resize(p.size());
 for(auto v:p)if(!std::isfinite(norm(v)))throw std::invalid_argument("position");
 for(auto t:tris){
  for(auto i:t)if(i>=p.size())throw std::invalid_argument("triangle index");
  Vec3 a=cross(p[t[1]]-p[t[0]],p[t[2]]-p[t[0]]);
  for(int j=0;j<3;++j){auto i=t[j];s.normal[i]+=a;s.area[i]+=norm(a)/6;
   s.adj[i].push_back(t[(j+1)%3]);s.adj[i].push_back(t[(j+2)%3]);}
 }
 for(std::size_t i=0;i<p.size();++i){
  s.normal[i]=unit(s.normal[i]);auto& a=s.adj[i];std::sort(a.begin(),a.end());
  a.erase(std::unique(a.begin(),a.end()),a.end());
  if(!std::isfinite(s.weight[i])||s.area[i]<=1e-24)s.weight[i]=0;
  s.weight[i]=std::clamp(s.weight[i],0.0,1.0);
 }return s;
}
// D10C: native visibility must be supplied by host; no inferred thickness.
inline void contact(Patch& s,std::size_t seed,double reach){
 std::vector<double> d(s.p.size(),std::numeric_limits<double>::infinity());
 using Q=std::pair<double,std::size_t>;
 std::priority_queue<Q,std::vector<Q>,std::greater<Q>> q;
 if(seed<s.p.size()&&s.weight[seed]>0&&reach>0){d[seed]=0;q.push({0,seed});}
 while(!q.empty()){auto [v,i]=q.top();q.pop();if(v!=d[i])continue;
  for(auto j:s.adj[i])if(s.weight[j]>0&&dot(s.normal[i],s.normal[j])>0){
   double x=v+norm(s.p[i]-s.p[j]);if(x<=reach&&x<d[j]){d[j]=x;q.push({x,j});}
  }
 }
 for(std::size_t i=0;i<d.size();++i)if(!std::isfinite(d[i]))s.weight[i]=0;
}
// D11.3: swept capsule with host-confirmed surface seeds and explicit
// visibility/write permission. Native weight is independent of traversal;
// visibility and zero permission are barriers. No automatic raycast or CCD.
struct SweptContact {
 std::vector<double> distance,falloff,weight;
};
inline SweptContact swept_contact(const Patch& s,const std::vector<std::size_t>& seeds,
 Vec3 start,Vec3 end,double radius,double reach,
 const std::vector<double>& visibility,const std::vector<double>& permission){
 const auto n=s.p.size();
 if(visibility.size()!=n||permission.size()!=n||
    !std::isfinite(radius)||radius<=0||!std::isfinite(reach)||reach<=0||
    !std::isfinite(norm(start))||!std::isfinite(norm(end)))
  throw std::invalid_argument("swept contact input");
 SweptContact out;
 out.distance.assign(n,std::numeric_limits<double>::infinity());
 out.falloff.resize(n);out.weight.resize(n);
 const Vec3 segment=end-start;
 const double length=norm(segment);
 if(!std::isfinite(length))throw std::invalid_argument("sweep length");
 const Vec3 axis=length>0?segment/length:Vec3{};
 std::vector<double> radial(n);
 auto allowed=[&](std::size_t i){return visibility[i]>0&&permission[i]>0&&s.area[i]>0&&radial[i]<radius;};
 for(std::size_t i=0;i<n;++i){
  if(!std::isfinite(visibility[i])||!std::isfinite(permission[i]))
   throw std::invalid_argument("contact visibility or permission");
  const Vec3 delta=s.p[i]-start;
  const double along=std::clamp(dot(delta,axis),0.0,length);
  radial[i]=norm(delta-axis*along);
  if(!std::isfinite(radial[i]))throw std::invalid_argument("contact radial distance");
 }
 using Q=std::pair<double,std::size_t>;
 std::priority_queue<Q,std::vector<Q>,std::greater<Q>> q;
 for(auto i:seeds){
  if(i>=n)throw std::invalid_argument("contact seed index");
  if(allowed(i)&&out.distance[i]!=0){out.distance[i]=0;q.push({0,i});}
 }
 while(!q.empty()){
  auto [d,i]=q.top();q.pop();
  if(d!=out.distance[i])continue;
  for(auto j:s.adj[i])if(allowed(j)){
   // Continuous bend penalty, no abrupt 90-degree normal gate.
   double bend=1+.5*(1-std::clamp(dot(s.normal[i],s.normal[j]),-1.0,1.0));
   double next=d+norm(s.p[j]-s.p[i])*bend;
   if(next<reach&&next<out.distance[j]){out.distance[j]=next;q.push({next,j});}
  }
 }
 auto smooth=[](double x){x=std::clamp(x,0.0,1.0);return x*x*(3-2*x);};
 for(std::size_t i=0;i<n;++i)if(std::isfinite(out.distance[i])){
  out.falloff[i]=smooth(1-radial[i]/radius)*smooth(1-out.distance[i]/reach);
  out.weight[i]=s.weight[i]*out.falloff[i]*std::clamp(visibility[i],0.0,1.0)*std::clamp(permission[i],0.0,1.0);
 }
 return out;
}
// D11.7 feature contact: native eligibility is a hard barrier; seed is
// the nearest eligible front-facing vertex to the current host hit point.
// Not an actual ray/face ID or a visibility solver. Admit vertical faces
// with a continuous orientation factor, exclude negative-facing normals.
struct FeatureContact {
 // Geometry-only contact envelope. It excludes the host falloff but includes
 // the continuous front/vertical orientation response.
 std::vector<double> contact;
 std::vector<double> weight;
 std::size_t seed=std::numeric_limits<std::size_t>::max();
};
// D11.9: keep the four independent decisions visible. Permission is a hard
// write/traversal barrier; contact is geometric; falloff is the host brush
// envelope; response is reserved for material scaling. The composed weight is
// the only value consumed by the material law. This helper intentionally
// preserves D11.8's product when response is one.
struct BrushField {
 std::vector<double> permission,contact,falloff,response,weight;
};
inline BrushField compose_brush_field(const std::vector<double>& permission,
 const std::vector<double>& contact,const std::vector<double>& falloff,
 const std::vector<double>& response={}){
 const std::size_t n=permission.size();
 if(contact.size()!=n||falloff.size()!=n||(!response.empty()&&response.size()!=n))
  throw std::invalid_argument("brush field size");
 BrushField out{permission,contact,falloff,response,std::vector<double>(n)};
 if(out.response.empty())out.response.assign(n,1.);
 for(std::size_t i=0;i<n;++i){
  for(double x:{out.permission[i],out.contact[i],out.falloff[i],out.response[i]})
   if(!std::isfinite(x))throw std::invalid_argument("brush field value");
  out.permission[i]=std::clamp(out.permission[i],0.,1.);
  out.contact[i]=std::clamp(out.contact[i],0.,1.);
  out.falloff[i]=std::clamp(out.falloff[i],0.,1.);
  out.response[i]=std::clamp(out.response[i],0.,1.);
  out.weight[i]=out.permission[i]*out.contact[i]*out.falloff[i]*out.response[i];
 }
 return out;
}
inline FeatureContact feature_contact(const Patch& s,Vec3 center,Vec3 axis,Vec3 start,double radius){
 if(!std::isfinite(radius)||radius<=0||!std::isfinite(norm(center))||
    !std::isfinite(norm(start))||!std::isfinite(norm(axis))||norm(axis)<=1e-20)
  throw std::invalid_argument("feature contact input");
 axis=unit(axis);
 FeatureContact out;out.contact.resize(s.p.size());out.weight.resize(s.p.size());
 std::vector<double> visibility(s.p.size(),1.),permission(s.p.size());
 double nearest=std::numeric_limits<double>::infinity();
 for(std::size_t i=0;i<s.p.size();++i)if(s.weight[i]>0&&s.area[i]>0&&norm(s.normal[i])>.5){
  double alignment=dot(s.normal[i],axis);
  if(alignment<-.025)continue;
  // A vertical wall receives 60% of its native envelope before spatial
  // falloff. Feather the small negative alignment tolerance to zero.
  double x=std::clamp((alignment+.025)/.025,0.,1.);
  permission[i]=x*x*(3-2*x)*(.6+.4*std::clamp(alignment/.3,0.,1.));
  double d=norm(s.p[i]-center);
  // A thin wall can be the entire hit region. Its near-vertical vertices
  // must be allowed to seed the same connected contact they can receive.
  if(permission[i]>0&&d<nearest){nearest=d;out.seed=i;}
 }
 if(out.seed==std::numeric_limits<std::size_t>::max()||nearest>=.65*radius){
  out.seed=std::numeric_limits<std::size_t>::max();return out;
 }
 // Limit continuity to a local stroke segment; never bridge a long jump.
 if(norm(center-start)>.75*radius)start=center;
 auto c=swept_contact(s,{out.seed},start,center,radius,1.5*radius+norm(center-start),visibility,permission);
 out.weight=std::move(c.weight);
 for(std::size_t i=0;i<s.p.size();++i)
  out.contact[i]=c.falloff[i]*std::clamp(permission[i],0.,1.);
 return out;
}
// D10D: scalar swept-volume proxy, not actual mass. Pairwise conservative.
inline std::vector<double> flux(const Patch& s,const std::vector<double>& pressure,double radius){
 if(pressure.size()!=s.p.size()||!std::isfinite(radius)||!(radius>0))throw std::invalid_argument("flux input");
 std::vector<double> h(s.p.size()),budget(s.p.size());
 for(double x:pressure)if(!std::isfinite(x))throw std::invalid_argument("pressure");
 for(std::size_t i=0;i<h.size();++i)budget[i]=s.area[i]*s.weight[i]*radius*.02;
 struct Transfer {std::size_t i,j;double f;};
 std::vector<Transfer> transfers;
 std::vector<double> demand(h.size());
 for(std::size_t i=0;i<h.size();++i)for(auto j:s.adj[i])if(i<j&&s.weight[i]>0&&s.weight[j]>0){
  double cap=std::min(budget[i],budget[j]);
  double f=std::clamp((pressure[i]-pressure[j])*cap,-cap,cap);
  transfers.push_back({i,j,f});demand[i]+=std::abs(f);demand[j]+=std::abs(f);
 }
 for(auto e:transfers){
  double scale=1;
  for(auto i:{e.i,e.j})if(demand[i]>budget[i])scale=std::min(scale,budget[i]/demand[i]);
  double f=e.f*scale;h[e.i]-=f;h[e.j]+=f;
 }
 for(std::size_t i=0;i<h.size();++i)if(s.area[i]>0)h[i]/=s.area[i];
 return h;
}
// D11.4: experimental finite-radius graph transport, mass-lumped
// surface quadrature. Coefficients have units of swept volume (L^3).
// A fixed physical support avoids shrinking transport to one edge upon
// refinement. Graph distance is approximate; not actual mass or volume.
// Rebuild after accepted geometry/contact changes; no host caching yet.
struct TransportStencil {
 struct Pair {std::size_t i,j;double coefficient;};
 std::size_t size=0;
 std::vector<double> area;
 std::vector<Pair> pairs;
};
inline TransportStencil transport_stencil(const Patch& s,double radius){
 if(!std::isfinite(radius)||radius<=0)throw std::invalid_argument("transport radius");
 TransportStencil out;out.size=s.p.size();out.area=s.area;
 const double reach=radius*.5;
 if(!(reach>0))throw std::invalid_argument("transport reach");
 const std::size_t n=out.size;
 std::vector<double> support(n),dist(n,std::numeric_limits<double>::infinity());
 std::vector<std::size_t> touched;
 using Q=std::pair<double,std::size_t>;
 std::vector<unsigned char> allowed(n);
 for(std::size_t i=0;i<n;++i)allowed[i]=s.weight[i]>0&&s.area[i]>0;
 struct Edge {std::size_t j;double length;};
 std::vector<std::vector<Edge>> edges(n);
 for(std::size_t i=0;i<n;++i)if(allowed[i]){
  edges[i].reserve(s.adj[i].size());
  for(auto j:s.adj[i])if(allowed[j])edges[i].push_back({j,norm(s.p[j]-s.p[i])});
 }
 // Reuse a single heap across sources.
 std::vector<Q> heap;heap.reserve(n);touched.reserve(n);
 auto push=[&](Q x){heap.push_back(x);std::push_heap(heap.begin(),heap.end(),std::greater<Q>{});};
 for(std::size_t i=0;i<n;++i){
  if(!allowed[i])continue;
  support[i]+=s.area[i]; // self quadrature, no self transfer
  dist[i]=0;touched.push_back(i);push({0,i});
  while(!heap.empty()){
   std::pop_heap(heap.begin(),heap.end(),std::greater<Q>{});
   auto [d,j]=heap.back();heap.pop_back();if(d!=dist[j])continue;
   if(j>i){
    double x=1-d/reach;
    double k=x*x*(3-2*x);
    out.pairs.push_back({i,j,k}); // temporary kernel, normalized below
    support[i]+=s.area[j]*k;
    support[j]+=s.area[i]*k;
   }
   for(auto edge:edges[j]){
    auto k=edge.j;
    double next=d+edge.length;
    if(next<reach&&next<dist[k]){
     if(!std::isfinite(dist[k]))touched.push_back(k);
     dist[k]=next;push({next,k});
    }
   }
  }
  for(auto j:touched)dist[j]=std::numeric_limits<double>::infinity();
  touched.clear();
 }
 for(auto& e:out.pairs){
  const auto i=e.i,j=e.j;
  double c=radius*.02*std::min(s.weight[i],s.weight[j])*s.area[i]*
           (s.area[j]/std::max(support[i],support[j]))*e.coefficient;
  if(!std::isfinite(c))throw std::overflow_error("transport coefficient");
  e.coefficient=c;
 }
 return out;
}
// Each pair contributes equal and opposite amounts simultaneously.
// max(row supports) ensures gross incident transfer <= area*weight*radius*.02
// for bounded pressure differences, without index-ordered capacity spending.
inline std::vector<double> transport_flux(const TransportStencil& s,
 const std::vector<double>& pressure){
 if(pressure.size()!=s.size)throw std::invalid_argument("transport pressure size");
 for(double x:pressure)if(!std::isfinite(x))throw std::invalid_argument("transport pressure");
 std::vector<double> h(s.size);
 for(auto e:s.pairs){
  const double f=std::clamp(pressure[e.i]-pressure[e.j],-1.0,1.0)*e.coefficient;
  h[e.i]-=f;h[e.j]+=f;
 }
 for(std::size_t i=0;i<h.size();++i)if(s.area[i]>0)h[i]/=s.area[i];
 return h;
}
// D11.5 opt-in distance cache. A fixed graph and edge length ratios
// L <= new_length/old_length <= U imply L*d0 <= d <= U*d0 for every
// shortest path, even if the shortest path itself changes. Cache candidates
// out to reach/min_ratio to cover pairs newly entering the support.
struct GeodesicCache {
 struct Edge {std::size_t i,j;double length;};
 struct Pair {std::size_t i,j;double distance;};
 std::vector<Tri> tris;
 std::vector<std::vector<std::size_t>> adj;
 std::vector<unsigned char> allowed;
 std::vector<Edge> edges;
 std::vector<Pair> pairs;
 double radius=0;
 static constexpr double min_ratio=.98,max_ratio=1.02;
};
inline GeodesicCache build_geodesic_cache(const Patch& s,double radius){
 if(!std::isfinite(radius)||radius<=0||!(radius*.5>0))throw std::invalid_argument("cache radius");
 const double reach=radius*.5/GeodesicCache::min_ratio*(1+1e-12);
 if(!std::isfinite(reach))throw std::invalid_argument("cache reach");
 GeodesicCache out;out.radius=radius;out.tris=s.tris;out.adj=s.adj;
 const auto n=s.p.size();out.allowed.resize(n);
 struct Edge {std::size_t j;double length;};
 std::vector<std::vector<Edge>> edges(n);
 for(std::size_t i=0;i<n;++i)out.allowed[i]=s.weight[i]>0&&s.area[i]>0;
 for(std::size_t i=0;i<n;++i)if(out.allowed[i])for(auto j:s.adj[i])if(out.allowed[j]){
  double length=norm(s.p[i]-s.p[j]);
  if(!std::isfinite(length))throw std::invalid_argument("cache edge");
  edges[i].push_back({j,length});
  if(i<j)out.edges.push_back({i,j,length});
 }
 using Q=std::pair<double,std::size_t>;
 std::vector<Q> heap;heap.reserve(n);
 std::vector<std::size_t> touched;touched.reserve(n);
 std::vector<double> dist(n,std::numeric_limits<double>::infinity());
 auto push=[&](Q q){heap.push_back(q);std::push_heap(heap.begin(),heap.end(),std::greater<Q>{});};
 for(std::size_t i=0;i<n;++i)if(out.allowed[i]){
  dist[i]=0;touched.push_back(i);push({0,i});
  while(!heap.empty()){
   std::pop_heap(heap.begin(),heap.end(),std::greater<Q>{});
   auto [d,j]=heap.back();heap.pop_back();if(d!=dist[j])continue;
   if(j>i)out.pairs.push_back({i,j,d});
   for(auto e:edges[j]){
    double next=d+e.length;
    if(next<reach&&next<dist[e.j]){
     if(!std::isfinite(dist[e.j]))touched.push_back(e.j);
     dist[e.j]=next;push({next,e.j});
    }
   }
  }
  for(auto j:touched)dist[j]=std::numeric_limits<double>::infinity();
  touched.clear();
 }
 return out;
}
inline bool geodesic_cache_ratios(const GeodesicCache& c,const Patch& s,double radius,
 double& lo,double& hi){
 if(!(c.radius>0)||c.allowed.size()!=s.p.size()||c.tris!=s.tris||c.adj!=s.adj)return false;
 for(std::size_t i=0;i<s.p.size();++i)
  if(bool(c.allowed[i])!=(s.weight[i]>0&&s.area[i]>0))return false;
 lo=std::numeric_limits<double>::infinity();hi=0;
 for(auto e:c.edges){
  double length=norm(s.p[e.i]-s.p[e.j]);
  if(e.length==0){if(length!=0)return false;continue;}
  double q=length/e.length;
  if(!std::isfinite(q)||q<GeodesicCache::min_ratio||q>GeodesicCache::max_ratio)return false;
  lo=std::min(lo,q);hi=std::max(hi,q);
 }
 if(hi==0){lo=hi=1;return radius<=c.radius/GeodesicCache::min_ratio;}
 // Pad the interval for floating point path accumulation. The analytical
 // bound is in real arithmetic, not a directed-rounding interval library.
 lo*=1-1e-12;hi*=1+1e-12;
 return lo>=GeodesicCache::min_ratio&&hi<=GeodesicCache::max_ratio&&
        radius<=c.radius*lo/GeodesicCache::min_ratio;
}
struct CachedFlux {
 std::vector<double> flux,error_bound;
 double relative_bound=0;
 bool reused=false;
};
inline CachedFlux evaluate_geodesic_cache(const GeodesicCache& c,const Patch& s,
 const std::vector<double>& pressure,double lo,double hi,double radius){
 const auto n=s.p.size();const double reach=radius*.5,mid=lo+(hi-lo)*.5;
 CachedFlux out;out.flux.resize(n);out.error_bound.resize(n);
 std::vector<double> low(n),high(n),est(n);
 for(std::size_t i=0;i<n;++i)if(c.allowed[i])low[i]=high[i]=est[i]=s.area[i];
 auto kernel=[&](double d){double x=std::clamp(1-d/reach,0.0,1.0);return x*x*(3-2*x);};
 auto add=[&](std::vector<double>& support,const GeodesicCache::Pair& e,double k){
  support[e.i]+=s.area[e.j]*k;support[e.j]+=s.area[e.i]*k;
 };
 for(auto e:c.pairs){
  add(low,e,kernel(e.distance*hi));add(high,e,kernel(e.distance*lo));add(est,e,kernel(e.distance*mid));
 }
 for(auto e:c.pairs){
  auto i=e.i,j=e.j;
  // Recompute current areas, fractional mask and normalization, not stale
  // coefficients. All three coefficient estimates are symmetric.
  double common=radius*.02*std::min(s.weight[i],s.weight[j])*s.area[i];
  auto coeff=[&](const std::vector<double>& support,double k){
   return common*(s.area[j]/std::max(support[i],support[j]))*k;
  };
  double a=coeff(high,kernel(e.distance*hi)),b=coeff(low,kernel(e.distance*lo));
  double v=coeff(est,kernel(e.distance*mid));
  double delta=std::clamp(pressure[i]-pressure[j],-1.0,1.0);
  double f=v*delta,err=std::max(std::abs(v-a),std::abs(b-v))*std::abs(delta);
  out.flux[i]-=f;out.flux[j]+=f;out.error_bound[i]+=err;out.error_bound[j]+=err;
 }
 double sq=0,esq=0;
 for(std::size_t i=0;i<n;++i)if(s.area[i]>0){
  out.flux[i]/=s.area[i];out.error_bound[i]/=s.area[i];
  sq+=s.area[i]*out.flux[i]*out.flux[i];
  esq+=s.area[i]*out.error_bound[i]*out.error_bound[i];
 }
 // ||exact|| >= ||estimate||-||error||, weighted by current area.
 // Thus this is an upper bound on relative L2 error if denominator > 0.
 double magnitude=std::sqrt(sq),error=std::sqrt(esq);
 out.relative_bound=error==0?0:(magnitude>error?error/(magnitude-error):std::numeric_limits<double>::infinity());
 return out;
}
inline CachedFlux cached_geodesic_flux(GeodesicCache& c,const Patch& s,
 const std::vector<double>& pressure,double radius,double tolerance=.02){
 if(pressure.size()!=s.p.size()||!std::isfinite(tolerance)||tolerance<0||tolerance>=1||
    !std::isfinite(radius)||radius<=0||!(radius*.5>0))throw std::invalid_argument("cached flux input");
 for(double x:pressure)if(!std::isfinite(x))throw std::invalid_argument("cache pressure");
 double lo=1,hi=1;
 if(geodesic_cache_ratios(c,s,radius,lo,hi)){
  auto result=evaluate_geodesic_cache(c,s,pressure,lo,hi,radius);
  if(result.relative_bound<=tolerance){result.reused=true;return result;}
 }
 // No uncertain result escapes. Build from the current geometry.
 // When evaluated with lo=hi=1, this is the reference distance kernel.
 auto next=build_geodesic_cache(s,radius);
 auto result=evaluate_geodesic_cache(next,s,pressure,1,1,radius);
 c=std::move(next);
 return result;
}
// D11.5 experimental aggregated quadrature; exact transport remains default.
// Each aggregate is connected within a spatial cell, and representative
// distances use the ORIGINAL allowed graph (no coarse-graph shortcuts).
// Pressure in [0,1] permits separable conservative fine reconstruction.
struct AggregateTransport {
 struct Group {std::vector<std::size_t> members;std::size_t representative=0;double mass=0;};
 struct Pair {std::size_t i,j;double kernel;};
 std::vector<Group> groups;
 std::vector<Pair> pairs;
 std::vector<double> support,area,weight;
 double radius=0;
};
inline AggregateTransport aggregate_transport(const Patch& s,double radius,double cell_fraction=.125,bool build_pairs=true){
 if(!std::isfinite(radius)||radius<=0||!std::isfinite(cell_fraction)||
    cell_fraction<=0||cell_fraction>.25||!(radius*cell_fraction>0)||!(radius*.5>0))
  throw std::invalid_argument("aggregate transport scale");
 AggregateTransport out;out.radius=radius;out.area=s.area;out.weight=s.weight;
 const auto n=s.p.size();const auto invalid=std::numeric_limits<std::size_t>::max();
 const double cell=radius*cell_fraction,reach=radius*.5;
 std::vector<std::size_t> group(n,invalid),rep(n,invalid),order;
 std::vector<std::array<double,3>> key(n);
 auto allowed=[&](std::size_t i){return s.weight[i]>0&&s.area[i]>0;};
 auto less=[&](std::size_t i,std::size_t j){
  const auto a=s.p[i],b=s.p[j];
  return std::array<double,3>{a.x,a.y,a.z}<std::array<double,3>{b.x,b.y,b.z};
 };
 // Use a geometry-derived origin; sorting makes distinct-position fixtures
 // insensitive to vertex numbering. Coincident vertex tie cases are unresolved.
 Vec3 origin{};bool have=false;
 for(std::size_t i=0;i<n;++i)if(allowed(i)){
  if(!have){origin=s.p[i];have=true;}
  origin.x=std::min(origin.x,s.p[i].x);origin.y=std::min(origin.y,s.p[i].y);origin.z=std::min(origin.z,s.p[i].z);
  order.push_back(i);
 }
 std::sort(order.begin(),order.end(),less);
 for(auto i:order){
  auto d=(s.p[i]-origin)/cell;
  if(!std::isfinite(norm(d)))throw std::overflow_error("aggregate cell coordinate");
  key[i]={std::floor(d.x),std::floor(d.y),std::floor(d.z)};
 }
 for(auto seed:order)if(group[seed]==invalid){
  auto id=out.groups.size();out.groups.emplace_back();
  auto& g=out.groups.back();g.members.push_back(seed);group[seed]=id;
  for(std::size_t k=0;k<g.members.size();++k)for(auto j:s.adj[g.members[k]])
   if(allowed(j)&&group[j]==invalid&&key[j]==key[seed]){group[j]=id;g.members.push_back(j);}
  std::sort(g.members.begin(),g.members.end(),less);
  Vec3 center{};
  for(auto i:g.members){double a=s.area[i]*s.weight[i];g.mass+=a;center+=(s.p[i]-origin)*a;}
  if(!(g.mass>0)||!std::isfinite(g.mass))throw std::overflow_error("aggregate mass");
  center=center/g.mass;
  g.representative=g.members.front();
  double best=norm(s.p[g.representative]-origin-center);
  for(auto i:g.members){double d=norm(s.p[i]-origin-center);if(d<best){best=d;g.representative=i;}}
  rep[g.representative]=id;
 }
 if(!build_pairs)return out; // internal grouping-only path for block study
 out.support.resize(out.groups.size());
 for(std::size_t i=0;i<out.groups.size();++i)out.support[i]=out.groups[i].mass;
 struct Edge {std::size_t j;double length;};
 std::vector<std::vector<Edge>> edges(n);
 for(auto i:order)for(auto j:s.adj[i])if(allowed(j))edges[i].push_back({j,norm(s.p[i]-s.p[j])});
 using Q=std::pair<double,std::size_t>;
 std::vector<Q> heap;
 std::vector<double> dist(n,std::numeric_limits<double>::infinity());
 std::vector<std::size_t> touched;
 heap.reserve(n);touched.reserve(n);
 auto push=[&](Q x){heap.push_back(x);std::push_heap(heap.begin(),heap.end(),std::greater<Q>{});};
 for(std::size_t a=0;a<out.groups.size();++a){
  auto i=out.groups[a].representative;
  dist[i]=0;touched.push_back(i);push({0,i});
  while(!heap.empty()){
   std::pop_heap(heap.begin(),heap.end(),std::greater<Q>{});
   auto [d,j]=heap.back();heap.pop_back();if(d!=dist[j])continue;
   auto b=rep[j];
   if(b!=invalid&&b>a){
    double x=1-d/reach,k=x*x*(3-2*x);
    out.pairs.push_back({a,b,k});
    out.support[a]+=out.groups[b].mass*k;
    out.support[b]+=out.groups[a].mass*k;
   }
   for(auto e:edges[j]){
    double next=d+e.length;
    if(next<reach&&next<dist[e.j]){
     if(!std::isfinite(dist[e.j]))touched.push_back(e.j);
     dist[e.j]=next;push({next,e.j});
    }
   }
  }
  for(auto j:touched)dist[j]=std::numeric_limits<double>::infinity();
  touched.clear();
 }
 return out;
}
// With effective area a_i=A_i*w_i, approximate pair coefficient
// a_i*a_j*K_ab/max(S_a,S_b). Sum over j <= a_i, so the per-vertex
// absolute exchange bound is .02*radius*w_i after division by A_i.
// Group mean + fine residual reconstruction avoids piecewise constant flow.
inline std::vector<double> aggregate_flux(const AggregateTransport& s,const std::vector<double>& pressure){
 if(pressure.size()!=s.area.size())throw std::invalid_argument("aggregate pressure size");
 for(double x:pressure)if(!std::isfinite(x)||x<0||x>1)throw std::invalid_argument("aggregate pressure range");
 const auto m=s.groups.size();
 std::vector<double> mean(m),row(m),moment(m),out(pressure.size());
 for(std::size_t a=0;a<m;++a){
  const auto& g=s.groups[a];
  for(auto i:g.members)mean[a]+=s.area[i]*s.weight[i]*pressure[i]/g.mass;
  row[a]=g.mass/s.support[a];moment[a]=row[a]*mean[a];
 }
 for(auto e:s.pairs){
  double c=e.kernel/std::max(s.support[e.i],s.support[e.j]);
  double ab=c*s.groups[e.j].mass,ba=c*s.groups[e.i].mass;
  row[e.i]+=ab;moment[e.i]+=ab*mean[e.j];
  row[e.j]+=ba;moment[e.j]+=ba*mean[e.i];
 }
 for(std::size_t a=0;a<m;++a)for(auto i:s.groups[a].members)
  out[i]=s.radius*.02*s.weight[i]*(moment[a]-row[a]*pressure[i]);
 return out;
}
// D11.5 certified block study: compress the exact stencil, not the graph
// metric. Build remains expensive; this measures safe compression potential.
// Only disjoint complete cross-group blocks with sufficiently uniform
// normalized coefficients are eligible. Incomplete blocks and near field
// remain exact. No approximation to masks or support normalization.
struct BlockTransport {
 struct Group {std::vector<std::size_t> members;double area=0;Vec3 lo{},hi{};};
 struct Block {std::size_t a,b;double coefficient;};
 std::vector<Group> groups;
 std::vector<Block> blocks;
 std::vector<TransportStencil::Pair> exact;
 std::vector<double> area;
 std::size_t represented_pairs=0;
};
inline BlockTransport block_transport(const Patch& s,double radius,double cell_fraction=.125,
 double tolerance=.05){
 if(!std::isfinite(tolerance)||tolerance<0||tolerance>=1)throw std::invalid_argument("block tolerance");
 auto partition=aggregate_transport(s,radius,cell_fraction,false);
 auto reference=transport_stencil(s,radius);
 BlockTransport out;out.area=s.area;
 const auto invalid=std::numeric_limits<std::size_t>::max();
 std::vector<std::size_t> ids(s.p.size(),invalid);
 for(const auto& g:partition.groups){
  auto id=out.groups.size();out.groups.emplace_back();
  auto& v=out.groups.back();v.members=g.members;v.lo=v.hi=s.p[g.members.front()];
  for(auto i:g.members){
   ids[i]=id;v.area+=s.area[i];auto p=s.p[i];
   v.lo.x=std::min(v.lo.x,p.x);v.lo.y=std::min(v.lo.y,p.y);v.lo.z=std::min(v.lo.z,p.z);
   v.hi.x=std::max(v.hi.x,p.x);v.hi.y=std::max(v.hi.y,p.y);v.hi.z=std::max(v.hi.z,p.z);
  }
 }
 using Key=std::pair<std::size_t,std::size_t>;
 struct Stat {std::size_t count=0;double lo=std::numeric_limits<double>::infinity(),hi=0;bool compress=false;};
 std::map<Key,Stat> stats;
 auto key=[&](std::size_t i,std::size_t j)->Key{return {std::min(ids[i],ids[j]),std::max(ids[i],ids[j])};};
 auto gap=[](double alo,double ahi,double blo,double bhi){return std::max(0.0,std::max(alo-bhi,blo-ahi));};
 auto blocks_well_separated=[&](Key k){
  const auto& a=out.groups[k.first];const auto& b=out.groups[k.second];
  return norm(Vec3{gap(a.lo.x,a.hi.x,b.lo.x,b.hi.x),gap(a.lo.y,a.hi.y,b.lo.y,b.hi.y),
                   gap(a.lo.z,a.hi.z,b.lo.z,b.hi.z)})>=radius*.125;
 };
 for(auto e:reference.pairs){
  auto k=key(e.i,e.j);
  if(k.first==k.second)continue;
  auto& v=stats[k];
  double c=e.coefficient/s.area[e.i]/s.area[e.j];
  ++v.count;v.lo=std::min(v.lo,c);v.hi=std::max(v.hi,c);
 }
 for(auto& entry:stats){
  const auto k=entry.first;auto& v=entry.second;
  const auto a=out.groups[k.first].members.size(),b=out.groups[k.second].members.size();
  // Division avoids overflow of a*b on large input.
  bool complete=a>0&&b>0&&v.count/a==b&&v.count%a==0;
  // A block must save at least one explicit pair. All its coefficients
  // are reduced by no more than tolerance, never increased.
  if(complete&&v.count>1&&blocks_well_separated(k)&&v.hi>0&&std::isfinite(v.hi)&&v.lo>0&&v.lo>=(1-tolerance)*v.hi){
   v.compress=true;out.blocks.push_back({k.first,k.second,v.lo});
   out.represented_pairs+=v.count;
  }
 }
 for(auto e:reference.pairs){
  auto k=key(e.i,e.j);
  auto it=stats.find(k);
  if(it==stats.end()||!it->second.compress)out.exact.push_back(e);
 }
 return out;
}
// Valid for pressure [0,1], where the exact pairwise clamp is inactive.
// Each accepted block is a positive rank-one pair coefficient A_i*A_j*c.
// Symmetry gives cancellation; coefficients <= exact preserve capacity.
inline std::vector<double> block_flux(const BlockTransport& s,const std::vector<double>& pressure){
 if(pressure.size()!=s.area.size())throw std::invalid_argument("block pressure size");
 for(double x:pressure)if(!std::isfinite(x)||x<0||x>1)throw std::invalid_argument("block pressure range");
 std::vector<double> out(pressure.size()),mean(s.groups.size()),row(s.groups.size()),moment(s.groups.size());
 for(auto e:s.exact){
  double f=e.coefficient*(pressure[e.i]-pressure[e.j]);out[e.i]-=f;out[e.j]+=f;
 }
 for(std::size_t a=0;a<s.groups.size();++a){
  const auto& g=s.groups[a];for(auto i:g.members)mean[a]+=s.area[i]*pressure[i]/g.area;
 }
 for(auto b:s.blocks){
  double ab=b.coefficient*s.groups[b.b].area,ba=b.coefficient*s.groups[b.a].area;
  row[b.a]+=ab;moment[b.a]+=ab*mean[b.b];
  row[b.b]+=ba;moment[b.b]+=ba*mean[b.a];
 }
 for(std::size_t i=0;i<out.size();++i)if(s.area[i]>0)out[i]/=s.area[i];
 for(std::size_t a=0;a<s.groups.size();++a)for(auto i:s.groups[a].members)
  out[i]+=moment[a]-row[a]*pressure[i];
 return out;
}
enum class Tool {Deposit,Press,Compact,Compress,Scrape,Push,Smear,Melt,Grab};
struct Material {double mobility=.55,compressibility=.2,viscosity=.4;};
// D11: stateless reference laws, no persistent plastic state yet.
inline std::vector<Vec3> target(const Patch& s,Tool tool,Material m,Vec3 direction,double radius,double dose){
 std::vector<Vec3> u(s.p.size());double a=radius*std::clamp(dose,0.0,.08)*std::clamp(m.mobility,0.0,1.0);
 if(!std::isfinite(radius)||radius<=0||!std::isfinite(dose)||!std::isfinite(norm(direction))||!std::isfinite(m.mobility)||!std::isfinite(m.compressibility)||!std::isfinite(m.viscosity))throw std::invalid_argument("dab input");
 dose=std::clamp(dose,0.0,1.0);
 auto h=flux(s,s.weight,radius);
 for(std::size_t i=0;i<u.size();++i){if(s.weight[i]<=0)continue;
  Vec3 n=s.normal[i],t=direction-n*dot(direction,n);double w=s.weight[i];
  switch(tool){
   case Tool::Deposit:u[i]=n*(a*w);break;
   case Tool::Scrape:u[i]=n*(-a*w);break;
   case Tool::Press:u[i]=n*(h[i]*dose-a*w*std::clamp(m.compressibility,0.0,1.0));break;
   case Tool::Compact:u[i]=n*(h[i]*dose*.35-a*w*std::clamp(m.compressibility,0.0,1.0)*1.5);break;
   case Tool::Compress:u[i]=n*(h[i]*dose-a*w*std::clamp(m.compressibility,0.0,1.0)*std::abs(dot(n,unit(direction))));break;
   case Tool::Push:u[i]=unit(t)*(a*w);break;
   case Tool::Smear:u[i]=t*(a*w*std::clamp(m.viscosity,0.0,1.0));break;
   case Tool::Melt:{Vec3 avg{};double count=0;for(auto j:s.adj[i])if(s.weight[j]>0){avg+=s.p[j]-s.p[i];++count;}if(count>0)u[i]=n*(std::clamp(dot(avg/count,n),-a,a)*w);break;}
   case Tool::Grab:u[i]=direction*(a*w);break;
  }
 }return u;
}
// D11.5: opt-in phenomenological material response. These are dimensionless
// reference parameters, not measured constitutive laws or bulk densities.
enum class MaterialKind {SoftClay,Clay,Wax,Wood,Stone};
struct MaterialLaw {
 double mobility,yield,compressibility,adhesion,hardening,compaction_rate;
};
inline MaterialLaw material_law(MaterialKind k){
 switch(k){
  case MaterialKind::SoftClay:return {.95,.05,.40,.65,.65,12};
  case MaterialKind::Clay:return {.65,.12,.25,.55,1.1,9};
  case MaterialKind::Wax:return {.48,.20,.08,.85,.45,4};
  case MaterialKind::Wood:return {.16,.45,.025,.24,1.4,2};
  case MaterialKind::Stone:return {.055,.70,.005,.12,2,1};
 }
 throw std::invalid_argument("material kind");
}
struct PlasticPoint {
 double deformation=0; // saturating local relative-motion proxy
 double compaction=0;  // saturating accepted inward-travel proxy
};
using PlasticState=std::vector<PlasticPoint>;
inline void validate_plastic(const Patch& s,const PlasticState& state,double radius){
 if(state.size()!=s.p.size()||!std::isfinite(radius)||radius<=0)
  throw std::invalid_argument("plastic state or radius");
 for(auto x:state)if(!std::isfinite(x.deformation)||!std::isfinite(x.compaction)||
    x.deformation<0||x.deformation>1||x.compaction<0||x.compaction>1)
  throw std::invalid_argument("plastic range");
}
inline bool compression_tool(Tool t){
 return t==Tool::Press||t==Tool::Compact||t==Tool::Compress;
}
inline bool plastic_tool(Tool t){
 return compression_tool(t)||t==Tool::Push||t==Tool::Smear||t==Tool::Melt;
}
// Pure proposal: state is read-only. Pressure includes yield and hardening.
// Transport is evaluated simultaneously with shared material mobility, not
// per-endpoint post-scaling (which would break the signed proxy).
inline std::vector<Vec3> material_target(const Patch& s,Tool tool,MaterialKind kind,
 const PlasticState& state,Vec3 direction,double radius,double dose,
 GeodesicCache* cache=nullptr,CachedFlux* diagnostics=nullptr,bool feature=false){
 validate_plastic(s,state,radius);
 if(!std::isfinite(dose)||!std::isfinite(norm(direction)))throw std::invalid_argument("material dab");
 auto m=material_law(kind);
 double dt=std::clamp(dose,0.0,.08);
 Vec3 dir=unit(direction);
 std::vector<double> drive(s.p.size());
 for(std::size_t i=0;i<drive.size();++i){
  double threshold=std::min(.99,m.yield*(1+m.hardening*(state[i].deformation+state[i].compaction)));
  drive[i]=std::clamp((s.weight[i]-threshold)/(1-threshold),0.0,1.0)/
           (1+m.hardening*(state[i].deformation+state[i].compaction));
 }
 if(diagnostics)*diagnostics=CachedFlux{};
 std::vector<double> flow(s.p.size());
 // Exactly uniform drive has zero pair flux even across masked regions.
 if(compression_tool(tool)&&dt>0&&!drive.empty()){
  double first=0;bool have=false,nonuniform=false;
  for(std::size_t i=0;i<drive.size();++i)if(s.weight[i]>0&&s.area[i]>0){
   if(!have){first=drive[i];have=true;}
   else if(drive[i]!=first){nonuniform=true;break;}
  }
  if(nonuniform){
   if(cache){
    auto result=cached_geodesic_flux(*cache,s,drive,radius);
    flow=result.flux;
    if(diagnostics)*diagnostics=std::move(result);
   }
   else flow=transport_flux(transport_stencil(s,radius),drive);
  }
 }
 std::vector<Vec3> result(s.p.size());
 for(std::size_t i=0;i<result.size();++i){
  if(s.weight[i]<=0)continue;
  const auto n=s.normal[i];const auto tangent=dir-n*dot(dir,n);
  const double a=radius*dt*m.mobility*drive[i],b=flow[i]*dt*m.mobility;
  if(feature&&compression_tool(tool)){
   // Transport remains along surface normal (same antisymmetric proxy).
   // Compression blends the inward local normal with the coherent tool
   // axis. Preserve the original flat response; avoid zero compression
   // at 90 degrees, without forcing opposite-facing motion.
   double alignment=std::clamp(dot(n,dir),0.,1.);
   Vec3 inward_axis=unit(n*.75+dir*.25);
   double sink=a*m.compressibility;
   if(tool==Tool::Compact)sink*=1.5;
   if(tool==Tool::Compress){
    // D11.8 preview: coherent axial compression on steep contact.
    // Keep normal transport separate; retain the flat-surface response.
    inward_axis=norm(dir)>0?dir:inward_axis;
    sink*=.85+.15*alignment;
   }
   result[i]=n*(tool==Tool::Compact?.35*b:b)-inward_axis*sink;
   continue;
  }
  switch(tool){
   case Tool::Deposit:result[i]=n*a;break;
   case Tool::Scrape:result[i]=n*(-a);break;
   case Tool::Press:result[i]=n*(b-a*m.compressibility);break;
   case Tool::Compact:result[i]=n*(.35*b-1.5*a*m.compressibility);break;
   case Tool::Compress:result[i]=n*(b-a*m.compressibility*std::abs(dot(n,dir)));break;
   case Tool::Push:result[i]=unit(tangent)*a;break;
   case Tool::Smear:result[i]=tangent*(a*m.adhesion);break;
   case Tool::Melt:{
    Vec3 avg{};double count=0;
    for(auto j:s.adj[i])if(s.weight[j]>0){avg+=s.p[j]-s.p[i];++count;}
    if(count>0)result[i]=n*std::clamp(dot(avg/count,n),-a,a);
    break;
   }
   case Tool::Grab:result[i]=dir*a;break;
  }
 }
 return result;
}
// Transactional state proposal from actual positions after write. The caller
// commits this returned state only together with the matching geometry.
// Same topology and vertex correspondence are required; no remap yet.
// Rejected/no-op vertices never gain plastic state. Rigid translation has
// zero relative deformation; compression is explicitly an inward-travel proxy.
inline PlasticState plastic_after_write(const Patch& before,const PlasticState& state,
 Tool tool,MaterialKind kind,double radius,const std::vector<Vec3>& after){
 validate_plastic(before,state,radius);
 if(after.size()!=before.p.size())throw std::invalid_argument("written position size");
 const auto m=material_law(kind);
 std::vector<Vec3> applied(after.size());
 for(std::size_t i=0;i<after.size();++i){
  applied[i]=after[i]-before.p[i];
  if(!std::isfinite(norm(after[i]))||!std::isfinite(norm(applied[i]))||
     (before.weight[i]<=0&&norm(applied[i])>0))throw std::invalid_argument("invalid or pinned write");
 }
 auto next=state;
 if(!plastic_tool(tool))return next;
 for(std::size_t i=0;i<after.size();++i){
  if(norm(applied[i])==0)continue;
  double strain=0;
  for(auto j:before.adj[i]){
   double length=norm(before.p[j]-before.p[i]);
   if(length>1e-20)strain=std::max(strain,norm(applied[i]-applied[j])/length);
  }
  next[i].deformation=std::clamp(state[i].deformation+(1-state[i].deformation)*
      (-std::expm1(-std::min(strain,1.0)*m.mobility)),0.0,1.0);
  if(compression_tool(tool)){
   double inward=std::max(0.0,-dot(before.normal[i],applied[i]))/radius;
   next[i].compaction=std::clamp(state[i].compaction+(1-state[i].compaction)*
       (-std::expm1(-std::min(inward,1.0)*m.compaction_rate)),0.0,1.0);
  }
 }
 return next;
}
// D10B: displacement-field Jacobi, pinned halo, existing global safety.
// Choose only a lower target-error feasible candidate; no geometry smoothing.
inline std::vector<Vec3> optimize(const Patch& s,std::vector<Vec3> desired,bool preserve_flux=false){
 if(desired.size()!=s.p.size())throw std::invalid_argument("target size");
 for(std::size_t i=0;i<desired.size();++i){
  if(!std::isfinite(norm(desired[i])))throw std::invalid_argument("nonfinite target");
  if(s.weight[i]<=0)desired[i]={};
 }
 auto safe=[&](std::vector<Vec3> u){double g=gradient_motion_scale_v31c(s.p,s.tris,u);
  for(auto& v:u)v=v*g;
  limit_local_contact(s.p,s.tris,u,true);return u;};
 auto error=[&](const std::vector<Vec3>& u){double e=0;for(std::size_t i=0;i<u.size();++i){auto v=u[i]-desired[i];e+=s.area[i]*dot(v,v);}return e;};
 auto volume=[&](const std::vector<Vec3>& u){double v=0;for(std::size_t i=0;i<u.size();++i)v+=s.area[i]*dot(s.normal[i],u[i]);return v;};
 auto best=safe(desired),u=desired;double e=error(best);
 const double reference=volume(best);
 double proxy_scale=0;
 for(std::size_t i=0;i<desired.size();++i){
  proxy_scale+=s.area[i]*(std::abs(dot(s.normal[i],desired[i]))+std::abs(dot(s.normal[i],best[i])));
 }
 const double proxy_tol=1e-10*std::max(1e-20,proxy_scale);
 // Compare D10A local attenuation before searching redistributed fields.
 auto local=desired;
 limit_local_gradient_v31d10a(s.p,s.tris,local);
 if((!preserve_flux||std::abs(volume(local)-reference)<=proxy_tol)&&error(local)<e){e=error(local);best=std::move(local);}
 // D11.2: recover target motion from a feasible field with simultaneous
 // incident-face constrained vertex steps. A triangle sees at most three
 // independent proposals; averaging with 1/3 preserves the convex gradient
 // bound in exact arithmetic. Revalidate projected area and gradient globally.
 // This is not a collision solver; signed-proxy mode retains the old path.
 if(e==0)return best;
 if(!preserve_flux){
  std::vector<std::vector<std::size_t>> incident(s.p.size());
  for(std::size_t k=0;k<s.tris.size();++k)for(auto i:s.tris[k])incident[i].push_back(k);
  const std::vector<Tri> one{{0,1,2}};
  std::vector<Vec3> q(3),m(3);
  for(int pass=0;pass<16;++pass){
   auto trial=best;
   for(std::size_t i=0;i<best.size();++i){
    if(s.weight[i]<=0||incident[i].empty())continue;
    const Vec3 d=desired[i]-best[i];
    if(norm(d)<=1e-12*std::max(1e-12,norm(desired[i])))continue;
    auto feasible=[&](double a){
     for(auto k:incident[i]){
      const auto& t=s.tris[k];
      ContactTriangle ct{};
      for(int j=0;j<3;++j){
       q[j]=s.p[t[j]];m[j]=best[t[j]];
       if(t[j]==i)m[j]+=d*a;
       ct.position[j]=q[j];ct.motion[j]=m[j];
      }
      if(gradient_motion_scale_v31c(q,one,m)<1.0||contact_triangle_scale(ct)<1.0)return false;
     }return true;
    };
    double lo=0,hi=1;
    if(feasible(1))lo=1;
    else for(int it=0;it<22;++it){double mid=(lo+hi)*.5;if(feasible(mid))lo=mid;else hi=mid;}
    trial[i]+=d*(lo/3.0);
   }
   auto candidate=safe(std::move(trial));
   double x=error(candidate);
   if(!(x<e))break;
   double improvement=e-x;
   best=std::move(candidate);e=x;
   if(improvement<=1e-12*e)break;
  }
 }
 // Preserve the feasible baseline signed normal-area proxy, not exact volume.
 auto balance=[&](std::vector<Vec3>& v){
  double a=0;for(std::size_t i=0;i<v.size();++i)if(s.weight[i]>0)a+=s.area[i]*s.weight[i]*dot(s.normal[i],s.normal[i]);
  if(a>0){double correction=(reference-volume(v))/a;
   for(std::size_t i=0;i<v.size();++i)if(s.weight[i]>0)v[i]+=s.normal[i]*(correction*s.weight[i]);}
 };
 for(int pass=0;pass<12;++pass){auto next=u;
  for(std::size_t i=0;i<u.size();++i)if(s.weight[i]>0&&!s.adj[i].empty()){
   Vec3 avg{};for(auto j:s.adj[i])avg+=u[j];
   next[i]=desired[i]*.35+avg*(.65/double(s.adj[i].size()));
  }u=std::move(next);auto trial=u;if(preserve_flux)balance(trial);auto candidate=safe(trial);
  if(preserve_flux&&std::abs(volume(candidate)-reference)>proxy_tol)continue;
  double x=error(candidate);
  if(x<e){e=x;best=std::move(candidate);}
 }return best;
}
// D11.7: compression has a physical sink, not a universal zero-volume
// constraint. Try local recovery only on a stalled target, and keep it
// only if both target error and signed-proxy error do not worsen versus
// strict reference. This does not establish actual volume or global CCD.
inline std::vector<Vec3> optimize_feature(const Patch& s,const std::vector<Vec3>& desired,bool* recovered=nullptr){
 if(recovered)*recovered=false;
 auto strict=optimize(s,desired,true);
 if(gradient_motion_scale_v31c(s.p,s.tris,desired)>=.95)return strict;
 auto local=optimize(s,desired,false);
 auto objective=[&](const std::vector<Vec3>& u){double v=0;for(std::size_t i=0;i<u.size();++i)v+=s.area[i]*dot(u[i]-desired[i],u[i]-desired[i]);return v;};
 auto proxy=[&](const std::vector<Vec3>& u){double v=0;for(std::size_t i=0;i<u.size();++i)v+=s.area[i]*dot(s.normal[i],u[i]);return v;};
 double target=proxy(desired),base=proxy(strict),candidate=proxy(local);
 if(objective(local)<objective(strict)&&std::abs(candidate-target)<=std::abs(base-target)+1e-14*std::max(1.,std::abs(target))){
  if(recovered)*recovered=true;
  return local;
 }
 return strict;
}
} // namespace hybrid_v31d11
} // namespace sculpt_physics
