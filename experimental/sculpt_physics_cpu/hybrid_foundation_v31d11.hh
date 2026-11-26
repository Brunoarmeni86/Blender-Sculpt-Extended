/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "brush_contact.hh"
#include <algorithm>
#include <queue>
#include <limits>
#include <stdexcept>
#include <functional>
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
 struct Raw {std::size_t i,j;double kernel;};
 std::vector<Raw> raw;
 std::vector<double> support(n),dist(n,std::numeric_limits<double>::infinity());
 std::vector<std::size_t> touched;
 using Q=std::pair<double,std::size_t>;
 auto allowed=[&](std::size_t i){return s.weight[i]>0&&s.area[i]>0;};
 for(std::size_t i=0;i<n;++i){
  if(!allowed(i))continue;
  support[i]+=s.area[i]; // self quadrature, no self transfer
  std::priority_queue<Q,std::vector<Q>,std::greater<Q>> q;
  dist[i]=0;touched.push_back(i);q.push({0,i});
  while(!q.empty()){
   auto [d,j]=q.top();q.pop();if(d!=dist[j])continue;
   if(j>i){
    double x=1-d/reach;
    double k=x*x*(3-2*x);
    raw.push_back({i,j,k});
    support[i]+=s.area[j]*k;
    support[j]+=s.area[i]*k;
   }
   for(auto k:s.adj[j])if(allowed(k)){
    double next=d+norm(s.p[k]-s.p[j]);
    if(next<reach&&next<dist[k]){
     if(!std::isfinite(dist[k]))touched.push_back(k);
     dist[k]=next;q.push({next,k});
    }
   }
  }
  for(auto j:touched)dist[j]=std::numeric_limits<double>::infinity();
  touched.clear();
 }
 for(auto e:raw){
  const auto i=e.i,j=e.j;
  double c=radius*.02*std::min(s.weight[i],s.weight[j])*s.area[i]*
           (s.area[j]/std::max(support[i],support[j]))*e.kernel;
  if(!std::isfinite(c))throw std::overflow_error("transport coefficient");
  out.pairs.push_back({i,j,c});
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
} // namespace hybrid_v31d11
} // namespace sculpt_physics
