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
// D10D: scalar swept-volume proxy, not actual mass. Pairwise conservative.
inline std::vector<double> flux(const Patch& s,const std::vector<double>& pressure,double radius){
 if(pressure.size()!=s.p.size()||!(radius>0))throw std::invalid_argument("flux input");
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
   case Tool::Compact:u[i]=n*(h[i]*dose-a*w*std::clamp(m.compressibility,0.0,1.0));break;
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
 for(std::size_t i=0;i<desired.size();++i)if(s.weight[i]<=0)desired[i]={};
 auto safe=[&](std::vector<Vec3> u){double g=gradient_motion_scale_v31c(s.p,s.tris,u);
  for(auto& v:u)v=v*g;
  limit_local_contact(s.p,s.tris,u,true);return u;};
 auto error=[&](const std::vector<Vec3>& u){double e=0;for(std::size_t i=0;i<u.size();++i){auto v=u[i]-desired[i];e+=s.area[i]*dot(v,v);}return e;};
 auto volume=[&](const std::vector<Vec3>& u){double v=0;for(std::size_t i=0;i<u.size();++i)v+=s.area[i]*dot(s.normal[i],u[i]);return v;};
 auto best=safe(desired),u=desired;double e=error(best);
 const double reference=volume(best);
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
  if(preserve_flux&&std::abs(volume(candidate)-reference)>1e-10*std::max(1e-20,std::abs(reference)+std::abs(volume(desired))))continue;
  double x=error(candidate);
  if(x<e){e=x;best=std::move(candidate);}
 }return best;
}
} // namespace hybrid_v31d11
} // namespace sculpt_physics
