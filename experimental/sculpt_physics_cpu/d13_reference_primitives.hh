/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace sculpt_physics::dedicated_d13 {
/* Standalone reference primitives. All dimensions in brush/world units.
 * These are not Blender editor dispatch or volumetric material engines. */
struct V {
  double x=0,y=0,z=0;
};
inline V operator+(V a,V b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline V operator-(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline V operator*(V a,double b){return {a.x*b,a.y*b,a.z*b};}
inline double dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline double norm(V a){return std::sqrt(dot(a,a));}
inline bool finite(V v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline V unit(V a){const double n=norm(a);return n>1e-12?a*(1./n):V{};}
inline double clamp01(double a){return std::clamp(a,0.,1.);}
struct Sample {V p,n;double influence=0,area=0;bool writable=false;};
struct Stroke {V from,to,normal,tangent;double radius=0,dose=0;};
inline bool valid(const Stroke& s){
  return finite(s.from)&&finite(s.to)&&finite(s.normal)&&finite(s.tangent)&&
    std::isfinite(s.radius)&&s.radius>1e-12&&std::isfinite(s.dose)&&s.dose>=0&&
    norm(s.normal)>1e-12;
}
inline double permission(const Sample& s){
  return s.writable&&finite(s.p)&&finite(s.n)&&std::isfinite(s.influence)?
    clamp01(s.influence):0.;
}
inline V closest(V p,const Stroke& s){
  const V d=s.to-s.from;const double q=dot(d,d);
  return q>1e-24?s.from+d*clamp01(dot(p-s.from,d)/q):s.from;
}
inline double rim(double r){const double t=clamp01(1.-r);return t*t*(3.-2.*t);}
inline double support(const Sample& p,const Stroke& s){
  if(!valid(s))return 0.;
  const double w=permission(p);
  const V n=unit(s.normal),d=p.p-closest(p.p,s);
  const double depth=dot(d,n);
  return w*rim(norm(d-n*depth)/s.radius)*rim(std::abs(depth)/s.radius)*
    rim(1.-clamp01(dot(unit(p.n),n)));
}

/* D13-B: one-sided plane scraper; persistent plane is caller-owned.
 * No removal beyond local contact reach. Dose is a maximum world step. */
inline V scrape(const Sample& p,const Stroke& s,V plane_point,V plane_normal){
  if(!valid(s)||!finite(plane_point)||!finite(plane_normal))return {};
  const V n=unit(plane_normal);
  const double height=dot(p.p-plane_point,n);
  return n*(-std::min(std::max(height,0.),s.dose)*support(p,s));
}

/* D13-B: swept V-profile incision; plane_point is the uncut surface origin.
 * A normalized cross-stroke direction defines a stable tool profile.
 * Surface displacement only: no topology changes or detached chips. */
inline V chisel(const Sample& p,const Stroke& s,V plane_point,V side,double half_width,double depth){
  if(!valid(s)||!finite(plane_point)||!finite(side)||!std::isfinite(half_width)||
     !std::isfinite(depth)||half_width<=0||depth<=0)return {};
  const V n=unit(s.normal),u=unit(side-n*dot(side,n));
  if(norm(u)<.5)return {};
  const double across=std::abs(dot(p.p-closest(p.p,s),u));
  const double profile=-depth*clamp01(1.-across/half_width);
  const double height=dot(p.p-plane_point,n);
  return n*(-std::min(std::max(height-profile,0.),s.dose)*support(p,s));
}

/* D13-C: conservative finite-volume transfer on an oriented contact chart.
 * Edge flux is a signed volume, not displacement. Positive a -> b.
 * The caller supplies available mobile volume and receiving capacity.
 * Reject invalid or non-writable edges. Budget all simultaneous flows,
 * then apply pairwise cancellation. No mass transported across masks. */
struct Cell {double area,available,capacity;bool writable;};
struct Flux {std::size_t a,b;double volume;};
struct FlowResult {std::vector<double> volume;std::vector<double> height;};
inline FlowResult conservative_transfer(const std::vector<Cell>& c,const std::vector<Flux>& edges){
  const auto n=c.size();
  std::vector<double> out(n,0.),in(n,0.),delta(n,0.),h(n,0.);
  for(const auto& v:c){
    if(!std::isfinite(v.area)||v.area<=0||!std::isfinite(v.available)||v.available<0||
       !std::isfinite(v.capacity)||v.capacity<0)throw std::invalid_argument("invalid cell");
  }
  for(const auto& e:edges){
    if(e.a>=n||e.b>=n||e.a==e.b||!std::isfinite(e.volume))throw std::invalid_argument("invalid flux");
    if(!c[e.a].writable||!c[e.b].writable)continue;
    const auto a=e.volume>=0?e.a:e.b,b=e.volume>=0?e.b:e.a;
    const double v=std::abs(e.volume);out[a]+=v;in[b]+=v;
  }
  for(const auto& e:edges){
    if(!c[e.a].writable||!c[e.b].writable)continue;
    const auto a=e.volume>=0?e.a:e.b,b=e.volume>=0?e.b:e.a;
    const double v=std::abs(e.volume);
    if(v==0.)continue;
    const double scale=std::min({1.,c[a].available/out[a],c[b].capacity/in[b]});
    const double q=v*scale;delta[a]-=q;delta[b]+=q;
  }
  for(std::size_t i=0;i<n;++i)h[i]=delta[i]/c[i].area;
  return {delta,h};
}

/* Distinct flux laws. Positive normal speed follows oriented edge a->b.
 * Inputs have world dimensions; mobility is chosen by the adapter.
 * Mud alone has a yield threshold. No claim of full rheological PDE. */
inline double mud_flux(double stress,double yield,double mobility,double face_length,double dt){
  if(!std::isfinite(stress)||!std::isfinite(yield)||!std::isfinite(mobility)||
     !std::isfinite(face_length)||!std::isfinite(dt)||yield<0||mobility<0||
     face_length<0||dt<0)return 0.;
  return std::copysign(std::max(0.,std::abs(stress)-yield),stress)*mobility*face_length*dt;
}
inline double push_flux(double normal_speed,double donor_height,double face_length,double dt){
  if(!std::isfinite(normal_speed)||!std::isfinite(donor_height)||!std::isfinite(face_length)||
     !std::isfinite(dt)||donor_height<0||face_length<0||dt<0)return 0.;
  return normal_speed*donor_height*face_length*dt;
}
inline double gather_flux(double radial_speed,double distance_a,double distance_b,double donor_height,
                          double face_length,double dt){
  if(!std::isfinite(distance_a)||!std::isfinite(distance_b)||!std::isfinite(radial_speed)||
     radial_speed<0||distance_a<0||distance_b<0||distance_a==distance_b)return 0.;
  return push_flux(distance_a>distance_b?radial_speed:-radial_speed,donor_height,face_length,dt);
}
} // namespace sculpt_physics::dedicated_d13
