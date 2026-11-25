/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
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
  // First eligible contact in this stroke; zero radius disables history.
  Vec3 contact_reference{};
  double contact_radius=0.0;
};
using ClaySurfaceTriangle=std::array<std::size_t,3>;
enum class ClaySurfaceAction { Buildup, Push, Gather, Spread, FusionDeposit, Compress, Compact, Press, PressureFlow, HybridFlow, Melt, Smear, Scrape };
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
  bool flow_fusion=false; // V20 Flow: weak relief blending alongside directed transport
  bool extended_material=false;
  bool density_independent=false; // V22 brush-scale transfer chart
  bool tool_v25=false; // V25 distinct contact and directional fold
  bool tool_v26=false; // experimental surface response; explicit native opt-in
  bool fast_v26b=false; // common connectivity and spatial lookup, including Deposit
  bool continuous_v26b=false; // retain supported portion of an interrupted trace
  bool contact_v27b=false; // broader contact/return profile
  bool stable_v28=false; // opt-in cumulative bounds and conservative proxy transport
  double melt_local_gravity_v27b=0.0; // opt-in object-local downhill
  bool has_tool_plane=false;
  double tool_plane_height=0.0; // relative to chart center, along chart axis
  double fold_gain=0.22, detail_retention=0.85, tool_depth=0.07;
  bool shape_transport=false; // V24 native-height remap and distinct contact operators
  bool smooth_proxy=false; // V23 positive quadratic B-spline transfer
  int proxy_cells_per_radius=8; // independent of native mesh density
  // Dimensionless artist controls, not calibrated rheological constants.
  double friction=0.7, viscosity=0.4, cohesion=0.6, stiffness=0.5;
  double plasticity=0.85, volume_preservation=0.9;
  double fusion=0.5;

};
// Bounded local pressure target; cumulative dose is in brush-radius units.
inline double progressive_press_depth_v26(double base,double cumulative_dose)
{
  return std::min(.25,base+.08*(1-std::exp(-3*std::max(0.0,cumulative_dose))));
}
struct ClaySurfaceResult {
  std::vector<Vec3> displacement;
  std::vector<double> projected_area;
  double signed_swept_volume=0.0;
  double transported_volume=0.0;
  double accepted_scale=1.0;
  std::size_t active=0;
  std::size_t transport_nodes=0;
  std::size_t trace_attempts=0,trace_rejected=0;
  std::size_t history_limited=0;
  double slope_scale=1.0;
};

/* Keep the original positive/negative projected budget ratio after
 * individual capacity clamps. This preserves zero net volume for balanced
 * actions and the configured sink fraction for Compact. Never amplify a
 * clipped target. A single-sign tool (Scrape) remains unilateral. */
inline void balance_capacity_v28(std::vector<double> &delta,const std::vector<double> &area,
                                 double original_pos,double original_neg)
{
  double pos=0,neg=0;
  for(std::size_t i=0;i<delta.size();++i){
    const double v=delta[i]*area[i];
    pos+=std::max(0.0,v);neg+=std::max(0.0,-v);
  }
  double ratio=1.0;
  if(original_pos>0)ratio=std::min(ratio,pos/original_pos);
  if(original_neg>0)ratio=std::min(ratio,neg/original_neg);
  for(double &v:delta){
    if(v>0)v*=pos>0?std::min(1.0,ratio*original_pos/pos):0.0;
    else if(v<0)v*=neg>0?std::min(1.0,ratio*original_neg/neg):0.0;
  }
}
inline double history_radius_fraction_v28(ClaySurfaceAction action)
{
  switch(action){
    case ClaySurfaceAction::Compact:return .16;
    case ClaySurfaceAction::Compress:return .24;
    case ClaySurfaceAction::Press:return .25;
    case ClaySurfaceAction::Scrape:return .35;
    default:return .28;
  }
}
/* Bound Euclidean cumulative motion, not a per-dab pressure counter.
 * If an external operation already exceeds the budget, allow recovery
 * but no worsening; reference is immutable for the duration of a stroke. */
inline double history_scale_v28(Vec3 current,Vec3 reference,Vec3 motion,double radius)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const Vec3 p=current-reference;
  const double a=dot(motion,motion);
  if(a<=0)return 1.0;
  const double p2=dot(p,p);
  const double cap2=std::max(p2,radius*radius);
  const double b=dot(p,motion);
  if(dot(p+motion,p+motion)<=cap2)return 1.0;
  const double headroom=std::max(0.0,cap2-p2);
  const double disc=std::sqrt(std::max(0.0,b*b+a*headroom));
  const double root=b>=0 ? (disc+b>0?headroom/(disc+b):0.0) : (disc-b)/a;
  return std::clamp(root,0.0,1.0);
}
/* Brush-scale finite-volume proxy. Each active vertex splats its
 * projected area * influence into bilinear or quadratic B-spline chart nodes. The exact
 * transpose of this transfer prolongates height increments. Thus
 * sum(A_i * dh_i) == sum(A_grid * dh_grid), including partial masks.
 * The grid spacing is a fraction of brush radius, never a mesh edge
 * length. This is a coarse surface model, not a volumetric MPM grid.
 * The caller supplies one connected, front-facing active chart. */
inline void coarse_clay_transport(
    const std::vector<Vec3> &xy,const std::vector<double> &h,
    const std::vector<double> &w,const std::vector<double> &area,
    const ClaySurfaceDab &dab,Vec3 axis,double floor,double travel,
    double mobility,double yield,double drag_gain,double pressure_gain,
    std::vector<double> &delta,ClaySurfaceResult &out)
{
  using Key=std::array<int,2>;
  struct Node { double area=0,raw=0,height=0,source=0,pressure=0,volume=0,change=0,outgoing=0,incoming=0; };
  struct Link {std::size_t a,b;double amount;};
  struct Weight {std::size_t id=0;double value=0;};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto unit=[](Vec3 p){const double l=std::hypot(p.x,p.y,p.z);return l>1e-12?p/l:Vec3{};};
  const Vec3 ex=unit(cross(std::abs(axis.z)<0.9?Vec3{0,0,1}:Vec3{0,1,0},axis));
  const Vec3 ey=cross(axis,ex);
  const double spacing=dab.radius/double(dab.proxy_cells_per_radius);
  const bool fusion=dab.action==ClaySurfaceAction::FusionDeposit;
  const bool compact=dab.action==ClaySurfaceAction::Compact;
  const bool melt=dab.action==ClaySurfaceAction::Melt;
  const bool smear=dab.action==ClaySurfaceAction::Smear;
  const bool gather=dab.action==ClaySurfaceAction::Gather;
  const bool pressure=dab.action==ClaySurfaceAction::Compress ||
      dab.action==ClaySurfaceAction::Press || dab.action==ClaySurfaceAction::PressureFlow ||
      dab.action==ClaySurfaceAction::HybridFlow || compact;
  std::map<Key,std::size_t> lookup;
  std::vector<Node> nodes;
  std::vector<std::array<Weight,9>> stencil(h.size());
  std::vector<std::array<std::size_t,2>> edges;
  const auto get=[&](Key key){
    auto it=lookup.find(key);
    if(it!=lookup.end())return it->second;
    auto id=nodes.size();lookup.emplace(key,id);nodes.emplace_back();return id;
  };
  for(std::size_t i=0;i<h.size();++i) {
    if(w[i]<=0)continue;
    const double x=dot(xy[i],ex)/spacing,y=dot(xy[i],ey)/spacing;
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>64||std::abs(y)>64)
      throw std::invalid_argument("Invalid coarse brush chart");
    const int ix=int(std::floor(x-(dab.smooth_proxy?.5:0.0)));
    const int iy=int(std::floor(y-(dab.smooth_proxy?.5:0.0)));
    const double fx=x-ix,fy=y-iy;
    const auto weights=[&](double f) {
      if(!dab.smooth_proxy)return std::array<double,3>{1-f,f,0};
      return std::array<double,3>{.5*(1.5-f)*(1.5-f),
                                  .75-(f-1)*(f-1),
                                  .5*(f-.5)*(f-.5)};
    };
    const auto wx=weights(fx),wy=weights(fy);
    double source=0,tool=0;
    if(fusion)source=dab.sign*travel*.10*w[i];
    if(compact)source=-dab.sign*(1-dab.volume_preservation)*
        std::min(travel*.065*w[i],.15*std::max(0.0,h[i]-floor));
    if(pressure) {
      const double gain=dab.action==ClaySurfaceAction::Compress?.30:
          dab.action==ClaySurfaceAction::PressureFlow?.28:
          dab.action==ClaySurfaceAction::HybridFlow?.20:.18;
      const double depth=dab.action==ClaySurfaceAction::Press?
          std::max(0.0,h[i]+.18*dab.radius):gain*dab.radius;
      tool=dab.sign*depth*w[i]*pressure_gain;
    }
    delta[i]=source; // add/sink at original resolution, transport on proxy
    for(int j=0;j<9;++j) {
      const int dx=j%3,dy=j/3;
      const double b=wx[dx]*wy[dy];
      if(b<=1e-14)continue;
      const auto id=get({ix+dx,iy+dy});
      stencil[i][j]={id,b};
      auto &g=nodes[id];
      const double m=area[i]*w[i]*b;
      g.area+=m;g.raw+=area[i]*b;
      g.height+=m*h[i];g.source+=m*source;
      g.pressure+=m*tool;
    }
  }
  // Connect occupied adjacent nodes within one connected active chart.
  // Coarse cells can bridge narrow mask holes within this component;
  // zero-influence vertices themselves never receive motion.
  for(const auto &entry:lookup) {
    for(Key k:{Key{entry.first[0]+1,entry.first[1]},Key{entry.first[0],entry.first[1]+1}}) {
      const auto it=lookup.find(k);
      if(it!=lookup.end())edges.push_back({std::min(entry.second,it->second),std::max(entry.second,it->second)});
    }
  }
  std::sort(edges.begin(),edges.end());
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  std::vector<Key> keys(nodes.size());
  for(const auto &e:lookup)keys[e.second]=e.first;
  for(auto &g:nodes) {
    if(g.area<=0||!std::isfinite(g.area))throw std::overflow_error("Invalid proxy area");
    g.height/=g.area;g.source/=g.area;g.pressure/=g.area;
    g.volume=std::max(0.0,g.height+g.source-floor)*g.area;
  }
  const Vec3 dir=unit(dab.direction-axis*dot(dab.direction,axis))*dab.sign;
  std::vector<Link> flux;
  for(const auto &e:edges) {
    auto a=e[0],b=e[1];
    const auto &u=nodes[a],&v=nodes[b];
    const double lo=std::min(u.area,v.area),hi=std::max(u.area,v.area);
    const double width=(2*lo/(1+lo/hi))/spacing;
    const double envelope=std::min(u.area/u.raw,v.area/v.raw);
    const Vec3 line=ex*double(keys[b][0]-keys[a][0])+ey*double(keys[b][1]-keys[a][1]);
    const Vec3 midpoint=ex*(.5*(keys[a][0]+keys[b][0])*spacing)+
                        ey*(.5*(keys[a][1]+keys[b][1])*spacing);
    const double align=dot(gather?unit(midpoint*(-1.0))*dab.sign:dir,line);
    const bool directed=gather||smear||dab.action==ClaySurfaceAction::Push||
        dab.action==ClaySurfaceAction::HybridFlow||dab.action==ClaySurfaceAction::PressureFlow;
    double f=0;
    if(directed) {
      const double available=align>=0?u.volume/u.area:v.volume/v.area;
      // Do not let a deepening virtual floor create ever larger flux.
      const double thickness=dab.stable_v28?std::min(available,.10*dab.radius):available;
      f=align*thickness*width*travel*mobility*envelope*drag_gain*
        (smear?.30:dab.action==ClaySurfaceAction::HybridFlow?.45:
         dab.action==ClaySurfaceAction::PressureFlow?.15:1.0);
    }
    // V26 surface-only Melt: gravity along the tangent chart in object coordinates.
    // This is a coarse relief flux, not thermal or volumetric simulation.
    if(melt && dab.tool_v26) {
      const double downhill=dot(Vec3{0,0,-1},line);
      const double thickness=downhill>=0?u.volume/u.area:v.volume/v.area;
      f+=downhill*thickness*width*travel*mobility*envelope*.65*
          (dab.contact_v27b?dab.melt_local_gravity_v27b:1.0);
    }
    if(pressure||fusion||melt||smear||dab.action==ClaySurfaceAction::Spread||
       (dab.action==ClaySurfaceAction::Push&&dab.flow_fusion)) {
      const double diff=(u.height+u.source+u.pressure)-(v.height+v.source+v.pressure);
      // The tool applies external forcing. Do not use the full
      // quiescent yield threshold against the relatively weak pressure
      // potential: that made Press/Compact nearly inert on smooth clay.
      const double contact_yield=melt?0.0:smear?yield*.15:pressure?yield*.25:yield;
      const double excess=std::max(0.0,std::abs(diff)-contact_yield*spacing);
      const double rate=melt?2.0:smear?1.5:fusion?dab.fusion:
          dab.action==ClaySurfaceAction::Push?.4*dab.fusion:1.0;
      f+=std::copysign(excess,diff)*width*
          std::min(travel*mobility*envelope*rate,spacing*.24);
    }
    if(!std::isfinite(f))throw std::overflow_error("Invalid proxy flux");
    if(f==0)continue;
    if(f<0){std::swap(a,b);f=-f;}
    nodes[a].outgoing+=f;nodes[b].incoming+=f;flux.push_back({a,b,f});
  }
  for(const auto &f:flux) {
    auto &u=nodes[f.a];auto &v=nodes[f.b];
    const double cap=u.volume*.25;
    double scale=std::min(1.0,cap/u.outgoing);
    if(dab.stable_v28){
      // A simultaneous donor and recipient CFL-like cap. All transfers
      // remain paired; no mass is created by a saturated receiver.
      const double local_cap=.035*dab.radius;
      scale=std::min({scale,local_cap*u.area/u.outgoing,
                            local_cap*v.area/v.incoming});
    }
    const double mass=f.amount*scale;
    u.change-=mass;v.change+=mass;out.transported_volume+=mass;
  }
  for(std::size_t i=0;i<h.size();++i) if(w[i]>0) {
    double d=0;
    for(const auto &b:stencil[i])if(b.value>0)
      d+=b.value*nodes[b.id].change/nodes[b.id].area;
    delta[i]+=w[i]*d;
  }
  out.transport_nodes=nodes.size();
}

/* V24: sample the current fine triangle surface instead of only its
 * coarse grid increments. Semi-Lagrangian height remap with a conservative
 * signed-volume correction (not a conservative characteristic remap).
 * No tangential vertex movement, no actual material IDs or topology edits.
 * Only triangles wholly in the connected writable component are sampled.
 * Segment probes reject unsupported traces; sub-probe holes are unresolved.
 */
inline void shape_clay_transport(
    const std::vector<Vec3> &xy,const std::vector<double> &h,
    const std::vector<double> &w,const std::vector<double> &area,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab,Vec3 axis,double travel,double mobility,
    std::vector<double> &delta,ClaySurfaceResult &out)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto unit=[&](Vec3 v){const double l=norm(v);return l>1e-12?v/l:Vec3{};};
  const Vec3 ex=unit(cross(std::abs(axis.z)<.9?Vec3{0,0,1}:Vec3{0,1,0},axis));
  const Vec3 ey=cross(axis,ex);
  const Vec3 dir=unit(dab.direction-axis*dot(dab.direction,axis));
  const Vec3 across=cross(axis,dir);
  // The acceleration index may refine with mesh density; the physical
  // sampling distance does not. Keep triangle searches local on dense meshes.
  const double spacing=dab.radius/std::clamp(std::sqrt(double(out.active))*.5,8.0,512.0);
  const double trace_spacing=dab.radius/24.0;
  using Key=std::array<int,2>;
  const auto key=[&](Vec3 p)->Key{
    return {int(std::floor(dot(p,ex)/spacing)),int(std::floor(dot(p,ey)/spacing))};
  };
  struct KeyHash {
    std::size_t operator()(const Key &k) const noexcept {
      return std::hash<int>{}(k[0]) ^ (std::hash<int>{}(k[1])+std::size_t(0x9e3779b9u)+(std::hash<int>{}(k[0])<<6)+(std::hash<int>{}(k[0])>>2));
    }
  };
  std::map<Key,std::vector<std::size_t>> bins;
  std::unordered_map<Key,std::vector<std::size_t>,KeyHash> fast_bins;
  std::vector<std::size_t> large_triangles;
  const bool remap=dab.action==ClaySurfaceAction::Push || dab.action==ClaySurfaceAction::Gather ||
      dab.action==ClaySurfaceAction::Smear || dab.action==ClaySurfaceAction::Melt ||
      dab.action==ClaySurfaceAction::Spread || dab.action==ClaySurfaceAction::HybridFlow ||
      dab.action==ClaySurfaceAction::PressureFlow;
  if(remap) for(std::size_t i=0;i<triangles.size();++i) {
    const auto &t=triangles[i];
    if(w[t[0]]<=0||w[t[1]]<=0||w[t[2]]<=0)continue;
    const auto a=key(xy[t[0]]),b=key(xy[t[1]]),c=key(xy[t[2]]);
    const int x0=std::min({a[0],b[0],c[0]}),x1=std::max({a[0],b[0],c[0]});
    const int y0=std::min({a[1],b[1],c[1]}),y1=std::max({a[1],b[1],c[1]});
    if(std::size_t(x1-x0+1)*std::size_t(y1-y0+1)>64)large_triangles.push_back(i);
    else for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y) {
      if(dab.fast_v26b)fast_bins[{x,y}].push_back(i);
      else bins[{x,y}].push_back(i);
    }
  }
  const auto sample=[&](Vec3 p,double &value) {
    const Key k=key(p);
    bool found=false;
    bool ambiguous=false;
    const auto scan=[&](const std::vector<std::size_t> &ids) {
    for(auto id:ids) {
      const auto &t=triangles[id];const auto a=xy[t[0]],b=xy[t[1]]-a,c=xy[t[2]]-a,d=p-a;
      const double det=dot(cross(b,c),axis);
      if(det<=1e-24)continue;
      const double u=dot(cross(d,c),axis)/det,v=dot(cross(b,d),axis)/det;
      if(u< -1e-9||v< -1e-9||u+v>1+1e-9)continue;
      const double z=(1-u-v)*h[t[0]]+u*h[t[1]]+v*h[t[2]];
      // Reject overlapping projected sheets with inconsistent heights.
      if(found && std::abs(z-value)>1e-7*dab.radius){ambiguous=true;return;}
      value=z;found=true;
    }
    };
    if(dab.fast_v26b){
      const auto it=fast_bins.find(k);
      if(it!=fast_bins.end())scan(it->second);
    }else{
      const auto it=bins.find(k);
      if(it!=bins.end())scan(it->second);
    }
    if(!ambiguous)scan(large_triangles);
    return found&&!ambiguous;
  };
  const auto trace=[&](Vec3 p,Vec3 shift,double &value) {
    if(norm(shift)<=1e-12)return false;
    ++out.trace_attempts;
    const int steps=std::max(1,int(std::ceil(norm(shift)/trace_spacing)));
    for(int k=1;k<=steps;++k){
      double next=value;
      if(!sample(p+shift*(double(k)/steps),next)){
        ++out.trace_rejected;
        // Keep only the already traversed, supported part. Never use a
        // failed/ambiguous sample or jump over a gap in the active chart.
        return dab.continuous_v26b && k>1;
      }
      value=next;
    }
    return true;
  };
  // Qualitative tool compliance; keep V24 unchanged. Cohesion and
  // stiffness attenuate plastic tool response, not the geometric mask.
  const double resistance=dab.tool_v25?(1-.45*dab.stiffness)*(1-.30*dab.cohesion):1;
  const double amount=std::min(.28*dab.radius,travel*(.35+1.65*mobility)*resistance);
  const double drag=amount*dab.friction;
  const auto smooth=[](double t){t=std::clamp(t,0.0,1.0);return t*t*(3-2*t);};
  const bool press=dab.action==ClaySurfaceAction::Press;
  const bool scrape=dab.action==ClaySurfaceAction::Scrape;
  const bool compact=dab.action==ClaySurfaceAction::Compact;
  const bool compress=dab.action==ClaySurfaceAction::Compress;
  std::vector<double> rim(w.size(),0.0);
  double removed=0,receiver=0,peak_rim=0;
  for(std::size_t i=0;i<w.size();++i)if(w[i]>0) {
    const auto p=xy[i];const double r=norm(p)/dab.radius;
    if(scrape) {
      // A unilateral cutter removes only material beyond its plane.
      const double plane=dab.has_tool_plane?dab.tool_plane_height:-dab.sign*dab.tool_depth*dab.radius;
      const double penetration=std::max(0.0,dab.sign*(h[i]-plane));
      delta[i]=-dab.sign*w[i]*std::min(penetration,amount*(dab.tool_v26?.85:.60));
    }
    else if(press||compact||compress) {
      const double inner=dab.contact_v27b?1-smooth((r-.30)/.49):
          1-smooth((r-.42)/(dab.tool_v26&&press?.37:.29));
      const double plane=dab.has_tool_plane?dab.tool_plane_height:-dab.sign*(dab.tool_v25?dab.tool_depth:.065)*dab.radius;
      double depth=press?std::max(0.0,dab.sign*(h[i]-plane)):amount*.30;
      if(dab.stable_v28 && !press)depth=std::min(depth,.08*dab.radius);
      delta[i]=-dab.sign*w[i]*inner*std::min(depth,amount*(dab.tool_v26&&press?.75:.55));
      // Broaden the receiving band while tapering its outer boundary.
      rim[i]=dab.contact_v27b?w[i]*smooth((r-.22)/.40)*(1-smooth((r-.78)/.22)):
          w[i]*smooth((r-(dab.tool_v26&&press?.38:.50))/.25);
      if(dab.stable_v28){
        // Reduce return into relief already above its first-contact level.
        // Shape is measured from geometry, not from requested displacement.
        // A smaller Compact rim makes it meaningfully different from Press.
        if(compact)rim[i]*=1-smooth((r-.65)/.35);
        // Avoid a narrow high receiving band even on an asymmetric mask.
        rim[i]*=1-smooth((r-.75)/.25);
      }
      peak_rim=std::max(peak_rim,rim[i]);
      removed-=delta[i]*area[i];receiver+=rim[i]*area[i];
    }
    else if(dab.action==ClaySurfaceAction::Push) {
      double z=h[i];
      if(trace(p,dir*(-dab.sign*drag),z))delta[i]=w[i]*(z-h[i]);
      if(dab.tool_v25 && norm(dir)>0) {
        // Signed front/back contact lobes: a rounded tool ploughs even
        // perfectly flat clay. Compact support and final signed budget
        // avoid a net source. Advection remains a separate component.
        const double u=dot(p,dir)/dab.radius;
        const double lateral=dot(p,across)/dab.radius;
        const double front=std::exp(-std::pow((u-.35)/.24,2)-std::pow(lateral/.45,2));
        const double rear=std::exp(-std::pow((u+.24)/.32,2)-std::pow(lateral/.45,2));
        delta[i]+=dab.sign*w[i]*amount*dab.fold_gain*(front-rear)*dab.friction;
      }
    }
    else if(dab.action==ClaySurfaceAction::Gather) {
      double z=h[i];
      const double taper=dab.tool_v26?1-smooth((r-.55)/.45):1.0;
      const Vec3 shift=unit(p)*std::min(drag*.7,norm(p)*.55)*dab.sign*taper;
      if(trace(p,shift,z))delta[i]=w[i]*(z-h[i]);
      // Smoothly extinguish the synthetic radial impulse at the brush boundary.
      // Final signed budget still balances the two signs.
      delta[i]+=dab.sign*w[i]*amount*.22*(1-3*r*r)*taper;
    }
    else {
      const bool smear=dab.action==ClaySurfaceAction::Smear ||
                       dab.action==ClaySurfaceAction::HybridFlow;
      const double reach=dab.radius*(smear?.20:.16);
      const Vec3 t=smear && norm(dir)>0?dir:ex;
      const Vec3 b=smear && norm(dir)>0?across:ey;
      // Multi-directional positive quadrature. Blend with the current
      // height to avoid four isolated copies of a narrow peak.
      double sum=2*h[i],count=2;
      for(int k=0;k<8;++k) {
        constexpr double pi=3.14159265358979323846;
        const double angle=pi*.25*k;
        const Vec3 offset=t*(reach*std::cos(angle))+b*(reach*(smear?.25:1)*std::sin(angle));
        double z=h[i];
        if(trace(p,offset,z)){sum+=z;++count;}
      }
      delta[i]=w[i]*(sum/count-h[i])*
          (1-std::exp(-amount/dab.radius*(smear?5.0:8.0)))*
          (dab.tool_v25&&smear?(1.0-.7*dab.detail_retention)*(.5+dab.fusion):1.0);
      if(smear) {
        double z=h[i];
        if(trace(p,dir*(-dab.sign*drag*.5),z))
          delta[i]+=w[i]*(z-h[i])*.55;
      }
      if(dab.action==ClaySurfaceAction::PressureFlow)
        delta[i]-=dab.sign*w[i]*amount*.15*(1-3*r*r);
    }
  }
  if(press||compact||compress) {
    const double fraction=compact?(dab.stable_v28?std::min(.40,dab.volume_preservation):dab.volume_preservation):1.0;
    double coefficient=receiver>1e-24?fraction*removed/receiver:0.0;
    const double peak=std::abs(coefficient)*peak_rim;
    const double peak_limit=amount*(dab.stable_v28?.30:.65);
    const double accepted=receiver<=1e-24?0.0:peak>peak_limit?peak_limit/peak:1.0;
    // Preserve the unreturned compaction fraction; reduce only the
    // conservative displacement when the receiving rim has no capacity.
    // Prevent a narrow mask opening from generating a high berm.
    for(std::size_t i=0;i<w.size();++i)
      delta[i]=delta[i]*((1-fraction)+fraction*accepted)+
               accepted*coefficient*rim[i];
  } else if(!scrape) {
    // Balance both signs without adding an artificial broad offset or
    // changing pinned vertices. A one-sided remap is rejected.
    double positive=0,negative=0;
    for(std::size_t i=0;i<w.size();++i) {
      const double v=delta[i]*area[i];
      positive+=std::max(0.0,v);negative+=std::max(0.0,-v);
    }
    const double budget=std::min(positive,negative);
    for(double &d:delta)d*=d>0?(positive>0?budget/positive:0):
                                 (negative>0?budget/negative:0);
  }
  double pos=0,neg=0;
  for(std::size_t i=0;i<w.size();++i){
    const double v=area[i]*delta[i];pos+=std::max(0.0,v);neg+=std::max(0.0,-v);
  }
  out.transported_volume=std::min(pos,neg);
  out.transport_nodes=(dab.fast_v26b?fast_bins.size():bins.size())+large_triangles.size();
}

using ClaySurfaceEdges=std::vector<std::array<std::size_t,2>>;

// Optional per-dab topology cache, populated only from validated triangles.
// Callers must clear it when changing the local vertex/triangle indexing.
inline ClaySurfaceResult solve_clay_surface(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab,
    ClaySurfaceEdges *dab_edges=nullptr)
{
  const auto norm=[](Vec3 a){return std::hypot(a.x,a.y,a.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{
      a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto interval=[](double x){return std::isfinite(x)&&x>=0&&x<=1;};
  if (!std::isfinite(norm(dab.center)) || !std::isfinite(norm(dab.axis)) ||
      norm(dab.axis)<=1e-12 || !std::isfinite(norm(dab.direction)) ||
      !std::isfinite(dab.radius) || dab.radius<=1e-12 ||
      !interval(dab.dose) || !std::isfinite(dab.intensity) || dab.intensity<0 ||
      dab.intensity>(dab.shape_transport?4.0:1.0) || !interval(dab.mobility) ||
      (dab.sign!=1 && dab.sign!=-1) ||
      !interval(dab.yield_slope) || !interval(dab.relative_slope_limit) ||
      dab.relative_slope_limit==0 ||
      !interval(dab.friction) || !interval(dab.viscosity) ||
      !interval(dab.cohesion) || !interval(dab.stiffness) ||
      !interval(dab.plasticity) || !interval(dab.volume_preservation) ||
      !interval(dab.fusion) || !interval(dab.fold_gain) || !interval(dab.detail_retention) ||
      !interval(dab.tool_depth) || !interval(dab.melt_local_gravity_v27b) || !std::isfinite(dab.tool_plane_height) ||
      (dab.tool_v26&&!dab.tool_v25) || (dab.tool_v25&&!dab.shape_transport) || dab.proxy_cells_per_radius<4 || dab.proxy_cells_per_radius>16) throw std::invalid_argument("Invalid clay surface dab");
  switch(dab.action) {
    case ClaySurfaceAction::Buildup: case ClaySurfaceAction::Push:
    case ClaySurfaceAction::Gather: case ClaySurfaceAction::Spread:
    case ClaySurfaceAction::FusionDeposit: case ClaySurfaceAction::Compress:
    case ClaySurfaceAction::Compact: case ClaySurfaceAction::Press:
    case ClaySurfaceAction::PressureFlow: case ClaySurfaceAction::HybridFlow:
    case ClaySurfaceAction::Melt: case ClaySurfaceAction::Smear: break;
    case ClaySurfaceAction::Scrape:
      if(!dab.tool_v25)throw std::invalid_argument("Scrape requires V25 tool path");
      break;
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
    if(!std::isfinite(norm(vertices[i].position)) || !interval(vertices[i].influence) ||
       (dab.stable_v28 && (!std::isfinite(vertices[i].contact_radius) || vertices[i].contact_radius<0 ||
                          !std::isfinite(norm(vertices[i].contact_reference)))))
      throw std::invalid_argument("Invalid clay surface vertex");
    const Vec3 offset=vertices[i].position-dab.center;
    h[i]=dot(offset,axis);
    xy[i]=tangent(offset);
    if(!std::isfinite(h[i]) || !std::isfinite(norm(xy[i])))
      throw std::overflow_error("Invalid clay chart");
    // Explicit depth bound avoids gathering the back side of a thin mesh.
    if(std::abs(h[i])<=dab.radius*0.5 &&
       ((dab.surface_contact && !dab.shape_transport) || norm(xy[i])<=dab.radius)) {
      w[i]=vertices[i].influence;
      if(dab.shape_transport) {
        const double t=std::clamp((.5-std::abs(h[i])/dab.radius)/.15,0.0,1.0);
        w[i]*=t*t*(3-2*t); // feather the chart depth boundary
      }
    }
  }
  ClaySurfaceEdges local_edges;
  auto &edges=dab_edges?*dab_edges:local_edges;
  const bool build_edges=edges.empty();
  if(build_edges)edges.reserve(triangles.size()*3);
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
    if(build_edges)for(int j=0;j<3;++j) {
      auto a=t[j],b=t[(j+1)%3];
      if(a>b) std::swap(a,b);
      edges.push_back({a,b});
    }
  }
  if(build_edges){
    std::sort(edges.begin(),edges.end());
    edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  }
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
  std::vector<bool> seen(n,false);
  if(dab.fast_v26b){
    // Same connected component as the BFS, with flat storage instead of
    // a heap-allocated adjacency list for each local vertex.
    std::vector<std::size_t> parent(n);
    for(std::size_t i=0;i<n;++i)parent[i]=i;
    const auto root=[&](std::size_t i){
      while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}
      return i;
    };
    for(const auto &e:edges)if(w[e[0]]>0 && w[e[1]]>0){
      const auto a=root(e[0]),b=root(e[1]);
      if(a!=b)parent[b]=a;
    }
    const auto component=root(seed);
    for(std::size_t i=0;i<n;++i)seen[i]=w[i]>0 && root(i)==component;
  }else{
    std::vector<std::vector<std::size_t>> adj(n);
    for(const auto &e:edges) if(w[e[0]]>0 && w[e[1]]>0) {
      adj[e[0]].push_back(e[1]);adj[e[1]].push_back(e[0]);
    }
    std::vector<std::size_t> queue{seed};seen[seed]=true;
    for(std::size_t k=0;k<queue.size();++k)
      for(auto j:adj[queue[k]]) if(!seen[j]){seen[j]=true;queue.push_back(j);}
  }
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
  const double mobility=dab.mobility*(dab.extended_material ?
      (0.25+0.75*dab.plasticity)/(1.0+2.0*dab.viscosity) : 1.0);
  const double yield=dab.yield_slope+(dab.extended_material ?
      0.10*dab.cohesion+0.08*dab.stiffness : 0.0);
  const double drag_gain=dab.extended_material?dab.friction:1.0;
  const double pressure_gain=dab.extended_material?(1.0-0.6*dab.stiffness):1.0;
  if(!std::isfinite(travel)) throw std::overflow_error("Invalid clay travel");
  if(dab.action==ClaySurfaceAction::Buildup) {
    for(std::size_t i=0;i<n;++i) if(w[i]>0)
      delta[i]=dab.sign*travel*0.10*w[i];
  } else if(dab.stable_v28 && dab.density_independent &&
            (dab.action==ClaySurfaceAction::Push || dab.action==ClaySurfaceAction::Gather ||
             dab.action==ClaySurfaceAction::Smear)){
    // Existing paired proxy flux replaces repeated triangle traces. This
    // is an experimental relief transfer, not real tangential material IDs.
    coarse_clay_transport(xy,h,w,out.projected_area,dab,axis,floor,travel,
                          mobility,yield,drag_gain,pressure_gain,delta,out);
  } else if(dab.shape_transport && dab.action!=ClaySurfaceAction::FusionDeposit &&
            !(dab.tool_v26 && dab.action==ClaySurfaceAction::Melt && dab.density_independent)) {
    shape_clay_transport(xy,h,w,out.projected_area,triangles,dab,axis,travel,mobility,delta,out);
  } else if(dab.density_independent &&
            (dab.smooth_proxy || dab.action!=ClaySurfaceAction::Gather)) {
    coarse_clay_transport(xy,h,w,out.projected_area,dab,axis,floor,travel,
                          mobility,yield,drag_gain,pressure_gain,delta,out);
  } else {
    if(dab.action==ClaySurfaceAction::Melt || dab.action==ClaySurfaceAction::Smear)
      throw std::invalid_argument("New material action needs V23 proxy");
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
        delta[i]=-dab.sign*(dab.extended_material?1.0-dab.volume_preservation:1.0)*
            std::min(travel*0.035*w[i],
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
        potential[i]+=dab.sign*penetration*w[i]*pressure_gain;
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
        const double excess=std::max(0.0,std::abs(slope)-yield*len);
        // Conductance bounded by local spacing for a stable explicit
        // transport step. Fusion is weaker than the dedicated pressure
        // actions and does not apply an ordinary position Laplacian.
        const double rate=fusion?(dab.extended_material?dab.fusion:0.35):1.0;
        f=std::copysign(excess,slope)*width*
             std::min(travel*mobility*envelope*rate,len*(dab.surface_contact?0.20:0.25));
        if(dab.action==ClaySurfaceAction::HybridFlow) {
          const double align=std::clamp(dot(unit(tangent(dab.direction))*dab.sign,line/len),-1.0,1.0);
          const double thickness=align>=0?volume[a]/aa:volume[b]/ab;
          f+=0.45*align*thickness*width*travel*mobility*envelope*drag_gain;
        }
      } else {
        const Vec3 field=dab.action==ClaySurfaceAction::Gather ?
          unit((xy[a]+xy[b])*(-0.5))*dab.sign : unit(tangent(dab.direction))*dab.sign;
        const double alignment=std::clamp(dot(field,line/len),-1.0,1.0);
        const double thickness=alignment>=0 ? volume[a]/aa:volume[b]/ab;
        f=alignment*thickness*width*travel*mobility*envelope*drag_gain;
        if(dab.surface_contact && dab.flow_fusion && dab.action==ClaySurfaceAction::Push) {
          const double dh=h[a]-h[b];
          const double excess=std::max(0.0,std::abs(dh)-yield*len);
          f+=std::copysign(excess,dh)*width*
              std::min(travel*mobility*envelope*(dab.extended_material?0.4*dab.fusion:0.20),len*0.10);
        }
      }
      if(!std::isfinite(f)) throw std::overflow_error("Clay surface flux overflow");
      if(f==0) continue;
      const auto from=f>0?a:b,to=f>0?b:a;
      outgoing[from]+=std::abs(f);flux.push_back({from,to,std::abs(f)});
    }
    for(const auto &f:flux) {
      if(!std::isfinite(outgoing[f.a])) throw std::overflow_error("Clay outgoing overflow");
      const double cap=volume[f.a]*(dab.extended_material?0.12:0.25);
      const double scale=outgoing[f.a]>cap?cap/outgoing[f.a]:1.0;
      const double v=f.v*scale;
      sent[f.a]+=v;received[f.b]+=v;out.transported_volume+=v;
    }
    for(std::size_t i=0;i<n;++i) if(w[i]>0)
      delta[i]+=(received[i]-sent[i])/out.projected_area[i];
  }
  const bool guarded=dab.stable_v28 && dab.action!=ClaySurfaceAction::Buildup &&
                     dab.action!=ClaySurfaceAction::FusionDeposit;
  if(guarded){
    double pos=0,neg=0;
    for(std::size_t i=0;i<n;++i){
      const double v=delta[i]*out.projected_area[i];
      pos+=std::max(0.0,v);neg+=std::max(0.0,-v);
    }
    for(std::size_t i=0;i<n;++i)if(delta[i]!=0 && vertices[i].contact_radius>0){
      const double limit=history_radius_fraction_v28(dab.action)*vertices[i].contact_radius;
      const double scale=history_scale_v28(vertices[i].position,vertices[i].contact_reference,axis*delta[i],limit);
      if(scale<1)++out.history_limited;
      delta[i]*=scale;
    }
    balance_capacity_v28(delta,out.projected_area,pos,neg);
    double final_pos=0,final_neg=0;
    for(std::size_t i=0;i<n;++i){
      const double v=delta[i]*out.projected_area[i];
      final_pos+=std::max(0.0,v);final_neg+=std::max(0.0,-v);
    }
    out.transported_volume=std::min(final_pos,final_neg);
  }
  // One common acceptance factor preserves the transport balance.
  // Limit increment of slopes across all edges including pinned exterior.
  // No Laplacian on original positions: existing details are not erased.
  if(dab.density_independent) {
    // Projected XY is unchanged: positive chart areas cannot invert in
    // this solve. Bound physical height travel instead of the shortest
    // fine-mesh edge. The editor still validates 3D incident triangles.
    double peak=0;
    for(double d:delta)peak=std::max(peak,std::abs(d));
    if(peak>dab.radius*.08)out.accepted_scale=dab.radius*.08/peak;
  }
  if(!dab.density_independent || guarded) for(const auto &e:edges) {
    const double increment=delta[e[0]]-delta[e[1]];
    const double diff=std::abs(increment);
    const double len=norm(xy[e[0]]-xy[e[1]]);
    if(diff>0) {
      const double existing=h[e[0]]-h[e[1]];
      // Allow reduction of existing slope without treating it as a new
      // instability. Still bound any increase of absolute height slope.
      const double recovery=dab.surface_contact && existing*increment<0 ?
          std::abs(existing):0.0;
      const double scale=guarded?
          // Permit steep existing edges to recover without making the
          // opposite slope worse. Never increase an already-steep edge.
          std::min((recovery+dab.relative_slope_limit*len)/diff,
                   std::max(0.0,(std::max(std::abs(existing),1.5*len)-
                                (increment>0?existing:-existing))/diff)):
          (recovery+dab.relative_slope_limit*len)/diff;
      out.accepted_scale=std::min(out.accepted_scale,scale);
      if(guarded)out.slope_scale=std::min(out.slope_scale,scale);
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
