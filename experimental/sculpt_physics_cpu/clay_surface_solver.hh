/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* V19-B: local projected-height clay solver. Fixed mesh or CCG chart.
 * Parallel motion along one fixed tool axis yields a measurable signed
 * swept-volume budget on the input triangulation. No hidden inventory:
 * usable thickness is estimated from current shape + virtual substrate.
 * This is NOT bulk MPM, collision detection, or persistent material state.
 * Caller supplies every incident triangle of every candidate vertex;
 * all other vertices are fixed. Front-facing connected chart only. */
struct ClaySurfaceVertex {
  Vec3 position{};
  double influence=0.0;
};
using ClaySurfaceTriangle=std::array<std::size_t,3>;
enum class ClaySurfaceAction { Buildup, Push, Gather, Spread, FusionDeposit, Compress, Compact, Press, PressureFlow, HybridFlow };
struct ClaySurfaceDab {
  ClaySurfaceAction action=ClaySurfaceAction::Buildup;
  Vec3 center{};
  Vec3 axis{0,0,1}; // always outward, independent of inversion
  Vec3 direction{};
  double radius=1.0;
  double dose=0.0; // normalized radius traveled or nominal dwell
  double intensity=1.0;
  double mobility=0.55;
  double sign=1.0;
  double yield_slope=0.10;
  double relative_slope_limit=0.35;
  bool surface_contact=false; // V20: recovery-friendly slope bound, native footprint

};
struct ClaySurfaceResult {
  std::vector<Vec3> displacement;
  std::vector<double> projected_area;
  double signed_swept_volume=0.0;
  double transported_volume=0.0;
  double accepted_scale=1.0;
  std::size_t active=0;
};

inline ClaySurfaceResult solve_clay_surface(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab)
{
  const auto norm=[](Vec3 a){return std::hypot(a.x,a.y,a.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{
      a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto interval=[](double x){return std::isfinite(x)&&x>=0&&x<=1;};
  if (!std::isfinite(norm(dab.center)) || !std::isfinite(norm(dab.axis)) ||
      norm(dab.axis)<=1e-12 || !std::isfinite(norm(dab.direction)) ||
      !std::isfinite(dab.radius) || dab.radius<=1e-12 ||
      !interval(dab.dose) || !interval(dab.intensity) || !interval(dab.mobility) ||
      (dab.sign!=1 && dab.sign!=-1) ||
      !interval(dab.yield_slope) || !interval(dab.relative_slope_limit) ||
      dab.relative_slope_limit==0) throw std::invalid_argument("Invalid clay surface dab");
  switch(dab.action) {
    case ClaySurfaceAction::Buildup: case ClaySurfaceAction::Push:
    case ClaySurfaceAction::Gather: case ClaySurfaceAction::Spread:
    case ClaySurfaceAction::FusionDeposit: case ClaySurfaceAction::Compress:
    case ClaySurfaceAction::Compact: case ClaySurfaceAction::Press:
    case ClaySurfaceAction::PressureFlow: case ClaySurfaceAction::HybridFlow: break;
    default: throw std::invalid_argument("Invalid clay surface action");
  }
  const Vec3 axis=dab.axis/norm(dab.axis);
  const auto tangent=[&](Vec3 v){return v-axis*dot(v,axis);};
  const auto unit=[&](Vec3 v){const double l=norm(v);return l>1e-12?v/l:Vec3{};};
  const std::size_t n=vertices.size();
  ClaySurfaceResult out;
  out.displacement.resize(n);
  out.projected_area.assign(n,0.0);
  std::vector<double> h(n),delta(n,0.0),w(n,0.0);
  std::vector<Vec3> xy(n);
  std::vector<bool> bad(n,false);
  for(std::size_t i=0;i<n;++i) {
    if(!std::isfinite(norm(vertices[i].position)) || !interval(vertices[i].influence))
      throw std::invalid_argument("Invalid clay surface vertex");
    const Vec3 offset=vertices[i].position-dab.center;
    h[i]=dot(offset,axis);
    xy[i]=tangent(offset);
    if(!std::isfinite(h[i]) || !std::isfinite(norm(xy[i])))
      throw std::overflow_error("Invalid clay chart");
    // Explicit depth bound avoids gathering the back side of a thin mesh.
    if(std::abs(h[i])<=dab.radius*0.5 && (dab.surface_contact || norm(xy[i])<=dab.radius))
      w[i]=vertices[i].influence;
  }
  std::vector<std::array<std::size_t,2>> edges;
  edges.reserve(triangles.size()*3);
  for(const auto &t:triangles) {
    if(t[0]>=n || t[1]>=n || t[2]>=n ||
       t[0]==t[1] || t[1]==t[2] || t[0]==t[2])
      throw std::invalid_argument("Invalid clay triangle");
    const Vec3 av=cross(vertices[t[1]].position-vertices[t[0]].position,
                        vertices[t[2]].position-vertices[t[0]].position)*0.5;
    const double projected=dot(av,axis),area=norm(av);
    if(!std::isfinite(projected)||!std::isfinite(area))
      throw std::overflow_error("Invalid clay triangle area");
    // Reject cells incident on a back-facing or nearly edge-on triangle.
    // Keeping projected orientation fixed prevents chart folds; it does
    // not guarantee no collisions with other geometry.
    if(area<=1e-24 || projected<=0.10*area) {
      for(auto i:t) bad[i]=true;
    } else {
      for(auto i:t) out.projected_area[i]+=projected/3.0;
    }
    for(int j=0;j<3;++j) {
      auto a=t[j],b=t[(j+1)%3];
      if(a>b) std::swap(a,b);
      edges.push_back({a,b});
    }
  }
  std::sort(edges.begin(),edges.end());
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  std::size_t seed=n;
  double nearest=std::numeric_limits<double>::infinity();
  for(std::size_t i=0;i<n;++i) {
    if(!std::isfinite(out.projected_area[i])) throw std::overflow_error("Clay dual area overflow");
    if(bad[i] || out.projected_area[i]<=1e-24) w[i]=0;
    if(w[i]>0) {
      const double d=norm(vertices[i].position-dab.center);
      if(d<nearest){nearest=d;seed=i;}
    }
  }
  if(seed==n || dab.dose==0 || dab.intensity==0) return out;
  // Only the connected component under the tip can exchange material.
  // Nonactive/pinned vertices must not serve as transport bridges.
  std::vector<std::vector<std::size_t>> adj(n);
  for(const auto &e:edges) if(w[e[0]]>0 && w[e[1]]>0) {
    adj[e[0]].push_back(e[1]);adj[e[1]].push_back(e[0]);
  }
  std::vector<bool> seen(n,false);
  std::vector<std::size_t> queue{seed};seen[seed]=true;
  for(std::size_t k=0;k<queue.size();++k)
    for(auto j:adj[queue[k]]) if(!seen[j]){seen[j]=true;queue.push_back(j);}
  double floor=h[seed];
  for(std::size_t i=0;i<n;++i){
    if(!seen[i])w[i]=0;
    else{floor=std::min(floor,h[i]);++out.active;}
  }
  // Finite effective substrate, recalculated from current chart per dab.
  // Lets Push work on existing clay as well as newly deposited relief.
  // Does not represent physical mass below the visible surface.
  floor-=0.12*dab.radius;
  const double travel=dab.radius*dab.dose*dab.intensity;
  if(!std::isfinite(travel)) throw std::overflow_error("Invalid clay travel");
  if(dab.action==ClaySurfaceAction::Buildup) {
    for(std::size_t i=0;i<n;++i) if(w[i]>0)
      delta[i]=dab.sign*travel*0.10*w[i];
  } else {
    struct Flux {std::size_t a,b; double v;};
    std::vector<Flux> flux;
    std::vector<double> outgoing(n,0.0),volume(n,0.0),sent(n,0.0),received(n,0.0);
    // V20 uses the geometry already produced by ANY preceding brush.
    // The projected height is the visible state, not a stale per-stroke
    // rest mesh or a hidden volume inventory. Pressure is a local
    // virtual tool potential, not calibrated physical pressure.
    const bool fusion=dab.action==ClaySurfaceAction::FusionDeposit;
    const bool compact=dab.action==ClaySurfaceAction::Compact;
    const bool contact=dab.action==ClaySurfaceAction::Compress ||
        dab.action==ClaySurfaceAction::Press ||
        dab.action==ClaySurfaceAction::PressureFlow ||
        dab.action==ClaySurfaceAction::HybridFlow || compact;
    std::vector<double> potential=h;
    for(std::size_t i=0;i<n;++i) if(w[i]>0) {
      if(fusion) delta[i]=dab.sign*travel*0.10*w[i];
      if(compact) {
        // Compressible preset: unlike volume-preserving Compress, allow
        // a bounded loss of projected volume. Ctrl reverses the sink.
        delta[i]=-dab.sign*std::min(travel*0.035*w[i],
                                  0.15*std::max(0.0,h[i]-floor));
      }
      volume[i]=std::max(0.0,h[i]+delta[i]-floor)*out.projected_area[i];
      potential[i]=h[i]+delta[i];
      if(contact) {
        const double gain=dab.action==ClaySurfaceAction::Compress ? 0.30 :
                          dab.action==ClaySurfaceAction::PressureFlow ? 0.22 :
                          dab.action==ClaySurfaceAction::HybridFlow ? 0.12 : 0.10;
        const double penetration=dab.action==ClaySurfaceAction::Press ?
            std::max(0.0,h[i]+0.08*dab.radius) : gain*dab.radius;
        potential[i]+=dab.sign*penetration*w[i];
      }
    }
    for(const auto &e:edges) {
      const auto a=e[0],b=e[1];
      if(w[a]==0 || w[b]==0) continue;
      const Vec3 line=xy[b]-xy[a];
      const double len=norm(line);
      if(len<=1e-12) continue;
      const double aa=out.projected_area[a],ab=out.projected_area[b];
      const double lo=std::min(aa,ab),hi=std::max(aa,ab);
      const double dual=2.0*(lo/(1.0+lo/hi));
      const double width=dual/len; // positive dual-interface approximation
      const double envelope=std::min(w[a],w[b]);
      double f=0;
      if(dab.action==ClaySurfaceAction::Spread || fusion || contact) {
        const double slope=potential[a]-potential[b];
        const double excess=std::max(0.0,std::abs(slope)-dab.yield_slope*len);
        // Conductance bounded by local spacing for a stable explicit
        // transport step. Fusion is weaker than the dedicated pressure
        // actions and does not apply an ordinary position Laplacian.
        const double rate=fusion?0.35:1.0;
        f=std::copysign(excess,slope)*width*
             std::min(travel*dab.mobility*envelope*rate,len*(dab.surface_contact?0.20:0.25));
        if(dab.action==ClaySurfaceAction::HybridFlow) {
          const double align=std::clamp(dot(unit(tangent(dab.direction))*dab.sign,line/len),-1.0,1.0);
          const double thickness=align>=0?volume[a]/aa:volume[b]/ab;
          f+=0.45*align*thickness*width*travel*dab.mobility*envelope;
        }
      } else {
        const Vec3 field=dab.action==ClaySurfaceAction::Gather ?
          unit((xy[a]+xy[b])*(-0.5))*dab.sign : unit(tangent(dab.direction))*dab.sign;
        const double alignment=std::clamp(dot(field,line/len),-1.0,1.0);
        const double thickness=alignment>=0 ? volume[a]/aa:volume[b]/ab;
        f=alignment*thickness*width*travel*dab.mobility*envelope;
      }
      if(!std::isfinite(f)) throw std::overflow_error("Clay surface flux overflow");
      if(f==0) continue;
      const auto from=f>0?a:b,to=f>0?b:a;
      outgoing[from]+=std::abs(f);flux.push_back({from,to,std::abs(f)});
    }
    for(const auto &f:flux) {
      if(!std::isfinite(outgoing[f.a])) throw std::overflow_error("Clay outgoing overflow");
      const double cap=volume[f.a]*0.25;
      const double scale=outgoing[f.a]>cap?cap/outgoing[f.a]:1.0;
      const double v=f.v*scale;
      sent[f.a]+=v;received[f.b]+=v;out.transported_volume+=v;
    }
    for(std::size_t i=0;i<n;++i) if(w[i]>0)
      delta[i]+=(received[i]-sent[i])/out.projected_area[i];
  }
  // One common acceptance factor preserves the transport balance.
  // Limit increment of slopes across all edges including pinned exterior.
  // No Laplacian on original positions: existing details are not erased.
  for(const auto &e:edges) {
    const double increment=delta[e[0]]-delta[e[1]];
    const double diff=std::abs(increment);
    const double len=norm(xy[e[0]]-xy[e[1]]);
    if(diff>0) {
      const double existing=h[e[0]]-h[e[1]];
      // Allow reduction of existing slope without treating it as a new
      // instability. Still bound any increase of absolute height slope.
      const double recovery=dab.surface_contact && existing*increment<0 ?
          2.0*std::abs(existing):0.0;
      out.accepted_scale=std::min(out.accepted_scale,
                                 (recovery+dab.relative_slope_limit*len)/diff);
    }
  }
  for(std::size_t i=0;i<n;++i) {
    const double d=delta[i]*out.accepted_scale;
    if(!std::isfinite(d)) throw std::overflow_error("Invalid clay surface displacement");
    out.displacement[i]=axis*d;
    out.signed_swept_volume+=d*out.projected_area[i];
  }
  out.transported_volume*=out.accepted_scale;
  if(!std::isfinite(out.signed_swept_volume)||!std::isfinite(out.transported_volume))
    throw std::overflow_error("Invalid clay surface volume");
  return out;
}

} // namespace sculpt_physics
