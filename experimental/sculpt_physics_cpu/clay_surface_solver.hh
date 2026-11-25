/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"
#include "brush_contact.hh"
#include <queue>
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
  double compaction_v29=0.0;
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
  bool live_surface_v30=false; // incremental current-geometry safety, no stroke-start displacement cap
  bool transport_v30b=false;
  bool contact_v30b=false;
  bool overlap_v30c=false;
  bool adaptive_v31=false; // adaptive selection, coherent per-dab direction
  bool gradient_v31c=false, melt_fallback_v31c=false;
  double average_surface_v31d=0.0; // virtual reference blend; never smooth actual geometry
  bool continuous_contact_v31d2=false;
  bool push_v30d=false; // stronger current-relief entrainment; Push only
  double push_gain_v30d=2.5;
  double transport_load_v30b=1.0;
  bool refine_v28b=false; // gradual capacities and curvature-aware Melt
  bool swept_v29=false;
  bool plastic_v29=false;
  Vec3 sweep_start{};
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
  double fine_transport_v30b=0.0;
  std::size_t chart_rejected_v30b=0,chart_recovered_v30b=0,depth_rejected_v30b=0;
  std::size_t component_rejected_v30b=0,local_limited_v30b=0;
  std::size_t patches_v30c=0,patch_budget_skipped_v30c=0;
  std::size_t adaptive_candidates_v31=0,adaptive_reached_v31=0,adaptive_deep_v31=0;
  std::size_t melt_fit_failures_v30b=0,melt_affine_fallbacks_v31c=0;
  std::size_t average_fits_v31d=0,average_recovered_v31d=0;
  double average_peak_v31d=0.0;
  double melt_residual_peak_v30b=0.0;
};

// Qualitative hardening state. Units are normal displacement / brush radius,
// not measured volumetric plastic strain. Inverse compression can loosen it.
inline double update_compaction_v29(double old,double normal_motion,double radius,double sign)
{
  if(!std::isfinite(old) || old<0 || old>1 || !std::isfinite(normal_motion) ||
     !std::isfinite(radius) || radius<=0 || (sign!=1 && sign!=-1))
    throw std::invalid_argument("Invalid V29 compaction");
  const double dose=std::clamp(-normal_motion*sign/radius,0.0,1.0);
  return sign>0 ? 1-(1-old)*std::exp(-4*dose) : old*std::exp(-4*dose);
}
inline double chart_distance_v29(Vec3 p,const ClaySurfaceDab &dab,Vec3 axis)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  if(dab.swept_v29){
    const Vec3 delta=dab.sweep_start-dab.center;
    const Vec3 a=delta-axis*dot(delta,axis);
    const double den=dot(a,a);
    const double t=den>1e-24?std::clamp(dot(p,a)/den,0.0,1.0):0.0;
    p=p-a*t;
  }
  return std::hypot(p.x,p.y,p.z);
}
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
/* Weighted least-squares quadratic in brush-radius coordinates.
 * A plane or quadratic chart is an equilibrium of the relief diffusion,
 * rather than being flattened because its height varies in the tool frame.
 * Singular/narrow masks fall back to zero flux instead of extrapolation. */
inline bool fit_quadratic_v28b(const std::vector<Vec3> &xy,const std::vector<double> &h,
                               const std::vector<double> &w,const std::vector<double> &area,
                               Vec3 ex,Vec3 ey,double radius,std::array<double,6> &coeff)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  double a[6][7]{};
  double total=0;
  for(std::size_t i=0;i<h.size();++i)if(w[i]>0){
    const double x=dot(xy[i],ex)/radius,y=dot(xy[i],ey)/radius;
    const double p[6]={1,x,y,x*x,x*y,y*y};
    const double m=area[i]*w[i];total+=m;
    for(int j=0;j<6;++j){
      for(int k=0;k<6;++k)a[j][k]+=m*p[j]*p[k];
      a[j][6]+=m*p[j]*h[i];
    }
  }
  if(!(total>0) || !std::isfinite(total))return false;
  for(int j=0;j<6;++j){
    int pivot=j;
    for(int k=j+1;k<6;++k)if(std::abs(a[k][j])>std::abs(a[pivot][j]))pivot=k;
    if(std::abs(a[pivot][j])<total*1e-9)return false;
    for(int k=j;k<7;++k)std::swap(a[j][k],a[pivot][k]);
    const double div=a[j][j];
    for(int k=j;k<7;++k)a[j][k]/=div;
    for(int i=0;i<6;++i)if(i!=j){
      const double f=a[i][j];
      for(int k=j;k<7;++k)a[i][k]-=f*a[j][k];
    }
  }
  for(int j=0;j<6;++j){coeff[j]=a[j][6];if(!std::isfinite(coeff[j]))return false;}
  return true;
}
/* Weighted affine regression in the actually supported directions.
 * Rank-one masks are allowed; unsupported slopes remain zero.
 * The centered 2x2 covariance avoids an arbitrary ridge on all terms
 * and preserves constant and affine surfaces on the sampled support. */
inline bool fit_affine_v31c(const std::vector<Vec3> &xy,const std::vector<double> &h,
                           const std::vector<double> &w,const std::vector<double> &area,
                           Vec3 ex,Vec3 ey,double radius,std::array<double,6> &coeff)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  double total=0,mx=0,my=0,mz=0;
  for(std::size_t i=0;i<h.size();++i)if(w[i]>0 && area[i]>0){
    const double m=area[i]*w[i];
    total+=m;mx+=m*dot(xy[i],ex)/radius;my+=m*dot(xy[i],ey)/radius;mz+=m*h[i];
  }
  if(!(total>0)||!std::isfinite(total))return false;
  mx/=total;my/=total;mz/=total;
  double xx=0,yy=0,xy_cov=0,xz=0,yz=0;
  for(std::size_t i=0;i<h.size();++i)if(w[i]>0 && area[i]>0){
    const double m=area[i]*w[i]/total;
    const double x=dot(xy[i],ex)/radius-mx,y=dot(xy[i],ey)/radius-my,z=h[i]-mz;
    xx+=m*x*x;yy+=m*y*y;xy_cov+=m*x*y;xz+=m*x*z;yz+=m*y*z;
  }
  if(!std::isfinite(xx)||!std::isfinite(yy)||!std::isfinite(xy_cov)||
     !std::isfinite(xz)||!std::isfinite(yz))return false;
  const double angle=.5*std::atan2(2*xy_cov,xx-yy);
  const double c=std::cos(angle),s=std::sin(angle);
  const double l0=c*c*xx+2*c*s*xy_cov+s*s*yy;
  const double l1=s*s*xx-2*c*s*xy_cov+c*c*yy;
  const double tol=std::max(1e-12,l0*1e-7);
  const double g0=l0>tol?(c*xz+s*yz)/l0:0;
  const double g1=l1>tol?(-s*xz+c*yz)/l1:0;
  const double gx=c*g0-s*g1,gy=s*g0+c*g1;
  coeff={mz-gx*mx-gy*my,gx,gy,0,0,0};
  for(double v:coeff)if(!std::isfinite(v))return false;
  return true;
}
/* V31D1: a robust brush-scale affine reference, built from a single
 * connected support selected by the caller. Two Huber reweighting
 * passes reduce the leverage of individual peaks. Only the virtual
 * reference is filtered; positions and physical volume stay unchanged. */
inline bool average_reference_v31d(const std::vector<Vec3> &xy,const std::vector<double> &h,
                                  const std::vector<double> &w,const std::vector<double> &area,
                                  Vec3 axis,double radius,std::vector<double> &reference,bool curved=false)
{
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  Vec3 ex=cross(std::abs(axis.z)<.9?Vec3{0,0,1}:Vec3{0,1,0},axis);
  const double len=std::hypot(ex.x,ex.y,ex.z);
  if(len<=1e-12)return false;
  ex=ex/len;const Vec3 ey=cross(axis,ex);
  std::array<double,6> coeff{};
  std::vector<double> robust=w;
  const auto predict=[&](std::size_t i){return coeff[0]+coeff[1]*dot(xy[i],ex)/radius+coeff[2]*dot(xy[i],ey)/radius;};
  for(int pass=0;pass<3;++pass){
    if(!fit_affine_v31c(xy,h,robust,area,ex,ey,radius,coeff))return false;
    if(pass<2)for(std::size_t i=0;i<h.size();++i)if(w[i]>0)
      robust[i]=w[i]*std::min(1.0,.15*radius/std::max(1e-24,std::abs(h[i]-predict(i))));
  }
  std::array<double,6> quad{};
  const bool curve_ok=curved && fit_quadratic_v28b(xy,h,robust,area,ex,ey,radius,quad);
  reference=h;
  for(std::size_t i=0;i<h.size();++i)if(w[i]>0){
    const double x=dot(xy[i],ex)/radius,y=dot(xy[i],ey)/radius;
    const double affine=predict(i);
    const double q=quad[0]+quad[1]*x+quad[2]*y+quad[3]*x*x+quad[4]*x*y+quad[5]*y*y;
    const double value=curve_ok?affine+.65*std::clamp(q-affine,-.25*radius,.25*radius):affine;
    if(!std::isfinite(value))return false;
    reference[i]=std::clamp(value,-.65*radius,.65*radius);
  }
  return true;
}
inline bool average_transport_v31d(const ClaySurfaceDab &dab)
{
  return dab.average_surface_v31d>0 && dab.live_surface_v30 && dab.stable_v28 &&
      (dab.action==ClaySurfaceAction::Push || dab.action==ClaySurfaceAction::Gather ||
       dab.action==ClaySurfaceAction::Smear || dab.action==ClaySurfaceAction::Melt);
}
inline double quadratic_height_v28b(const std::array<double,6> &c,double x,double y)
{
  return c[0]+c[1]*x+c[2]*y+c[3]*x*x+c[4]*x*y+c[5]*y*y;
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
    std::vector<double> &delta,ClaySurfaceResult &out,
    const std::vector<std::array<std::size_t,2>> *fine_edges=nullptr)
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
  std::array<double,6> background{};
  const bool live_transport=dab.transport_v30b && (gather||smear||dab.action==ClaySurfaceAction::Push);
  const bool plough=dab.push_v30d && live_transport && dab.action==ClaySurfaceAction::Push;
  const bool detrend=dab.refine_v28b && melt;
  const bool quadratic_ok=!(detrend||(live_transport&&!plough)) || fit_quadratic_v28b(xy,h,w,area,ex,ey,dab.radius,background);
  const bool affine_fallback=detrend && !quadratic_ok && dab.melt_fallback_v31c &&
      fit_affine_v31c(xy,h,w,area,ex,ey,dab.radius,background);
  const bool fit_ok=quadratic_ok || affine_fallback;
  if(affine_fallback)++out.melt_affine_fallbacks_v31c;
  const auto residual=[&](std::size_t i){
    // A broad pile is part of the material Push must entrain. Unlike Melt,
    // do not subtract a quadratic fit through the pile itself. Use the
    // current connected chart's low support, with bounded effective depth.
    if(plough)return std::clamp(h[i]-(floor+.12*dab.radius),0.0,.30*dab.radius);
    if((detrend||live_transport) && fit_ok)
      return (affine_fallback?.35:1.0)*(h[i]-quadratic_height_v28b(background,dot(xy[i],ex)/dab.radius,dot(xy[i],ey)/dab.radius));
    return detrend?0.0:h[i]-(live_transport?floor+.12*dab.radius:0.0);
  };
  if(detrend){
    if(!quadratic_ok)++out.melt_fit_failures_v30b;
    if(fit_ok)for(std::size_t i=0;i<h.size();++i)if(w[i]>0)
      out.melt_residual_peak_v30b=std::max(out.melt_residual_peak_v30b,std::abs(residual(i))/dab.radius);
  }
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
      const double relief=residual(i);
      g.height+=m*relief;g.source+=m*source;
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
    g.volume=(plough?std::clamp(.12*dab.radius+.85*g.height,.12*dab.radius,.36*dab.radius):
              live_transport?std::clamp(.08*dab.radius+.7*g.height,.015*dab.radius,.20*dab.radius):
              detrend?.10*dab.radius:std::max(0.0,g.height+g.source-floor))*g.area;
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
    Vec3 field=gather?unit(midpoint*(-1.0))*dab.sign:dir;
    if(live_transport){
      Vec3 q=midpoint;
      if(dab.swept_v29){
        const Vec3 segment=dab.sweep_start-dab.center;
        const Vec3 tangent=segment-axis*dot(segment,axis);
        const double den=dot(tangent,tangent);
        if(den>1e-24)q=q-tangent*std::clamp(dot(q,tangent)/den,0.0,1.0);
      }
      const double r=std::hypot(q.x,q.y,q.z)/dab.radius;
      const double taper=std::max(0.0,1-r*r);
      if(gather)field=q*(-dab.sign/dab.radius)*(2.2*taper*taper);
      else if(plough){
        // Broad central translation, smoothly decelerating at the leading
        // edge. The existing native envelope still controls mask/alpha.
        const double t=std::clamp((r-.55)/.45,0.0,1.0);
        const double contact=1-t*t*(3-2*t);
        field=dir*(contact*(.75+.25*dab.transport_load_v30b));
      }else field=dir*(taper*taper*(smear?1.0:dab.transport_load_v30b));
    }else if(dab.refine_v28b && gather){
      // A regular radial velocity vanishes at the center instead of
      // converging at finite speed into a singular point.
      const double r=std::hypot(midpoint.x,midpoint.y,midpoint.z)/dab.radius;
      field=field*std::clamp(r/.35,0.0,1.0);
    }
    const double align=dot(field,line);
    const bool directed=gather||smear||dab.action==ClaySurfaceAction::Push||
        dab.action==ClaySurfaceAction::HybridFlow||dab.action==ClaySurfaceAction::PressureFlow;
    double f=0;
    if(directed) {
      const double available=align>=0?u.volume/u.area:v.volume/v.area;
      // Do not let a deepening virtual floor create ever larger flux.
      const double thickness=dab.stable_v28 && !live_transport?std::min(available,.10*dab.radius):available;
      // The proxy splat and transpose already encode native influence.
      // Keep an additional square-root envelope for Push instead of
      // multiplying the full falloff yet again. Zero mask stays zero.
      const double contact_envelope=plough?std::sqrt(std::clamp(envelope,0.0,1.0)):envelope;
      f=align*thickness*width*travel*mobility*contact_envelope*drag_gain*
        (plough?dab.push_gain_v30d:smear?.30:dab.action==ClaySurfaceAction::HybridFlow?.45:
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
    if(dab.stable_v28 && !fusion){
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
  if(live_transport && (smear||plough) && fine_edges){
    // Paired fine residual transfers on mesh edges, without another resampling trace.
    std::vector<double> detail(h.size()),incoming(h.size()),outgoing(h.size()),change(h.size());
    std::vector<Link> fine;
    fine.reserve(fine_edges->size());
    for(std::size_t i=0;i<h.size();++i)if(w[i]>0){
      double reconstructed=0,total=0;
      for(const auto &b:stencil[i])if(b.value>0){reconstructed+=b.value*nodes[b.id].height;total+=b.value;}
      detail[i]=residual(i)-(total>0?reconstructed/total:0.0);
    }
    for(const auto &edge:*fine_edges){
      auto a=edge[0],b=edge[1];
      if(w[a]<=0||w[b]<=0)continue;
      const Vec3 line=xy[b]-xy[a];const double len=std::hypot(line.x,line.y,line.z);
      if(len<=1e-12)continue;
      const double lo=std::min(area[a],area[b]),hi=std::max(area[a],area[b]);
      if(lo<=0)continue;
      const double dual=2*lo/(1+lo/hi),align=dot(dir,line/len);
      const double step=std::min((plough?.24:.18)*len,travel*mobility*std::min(w[a],w[b])*drag_gain*(plough?dab.push_gain_v30d:1.0));
      const double upwind=align>=0?detail[a]:detail[b];
      const double diffusion=(detail[a]-detail[b])*(plough?.06:(.12+.28*align*align)*(1-.6*dab.detail_retention));
      double f=((plough?1.0:.65)*align*upwind+diffusion)*dual/len*step;
      if(!std::isfinite(f))throw std::overflow_error("V30B fine flux overflow");
      if(f==0)continue;
      if(f<0){std::swap(a,b);f=-f;}
      outgoing[a]+=f;incoming[b]+=f;fine.push_back({a,b,f});
    }
    for(const auto &f:fine){
      const double limit=.015*dab.radius;
      const double mass=f.amount*std::min({1.0,limit*area[f.a]*w[f.a]/outgoing[f.a],limit*area[f.b]*w[f.b]/incoming[f.b]});
      change[f.a]-=mass;change[f.b]+=mass;out.fine_transport_v30b+=mass;
    }
    for(std::size_t i=0;i<h.size();++i)if(w[i]>0)delta[i]+=change[i]/area[i];
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
  double rim_radius=1.0;
  if(dab.contact_v30b && (press||compact||compress)){
    double max_r=0;
    for(std::size_t i=0;i<w.size();++i)if(w[i]>.02)
      max_r=std::max(max_r,chart_distance_v29(xy[i],dab,axis)/dab.radius);
    rim_radius=std::clamp(max_r/.88,.25,1.0);
  }
  const auto penetration_v30b=[&](double height,double plane,double r){
    double z=dab.sign*height,p=dab.sign*plane;
    if(dab.contact_v30b){
      // Follow shallow local depressions, bounded and feathered at the rim.
      const double recover=std::clamp(p-z+.35*dab.tool_depth*dab.radius,0.0,.20*dab.radius);
      p-=recover*(1-smooth((r-.42)/.45));
    }
    return std::max(0.0,z-p);
  };
  for(std::size_t i=0;i<w.size();++i)if(w[i]>0) {
    const auto p=xy[i];const double r=chart_distance_v29(p,dab,axis)/dab.radius;
    if(scrape) {
      // A unilateral cutter removes only material beyond its plane.
      const double plane=dab.has_tool_plane?dab.tool_plane_height:-dab.sign*dab.tool_depth*dab.radius;
      const double penetration=penetration_v30b(h[i],plane,r);
      delta[i]=-dab.sign*w[i]*std::min(penetration,amount*(dab.tool_v26?.85:.60));
    }
    else if(press||compact||compress) {
      const double rr=r/rim_radius;
      const double inner=dab.contact_v27b?1-smooth((rr-.30)/.49):
          1-smooth((r-.42)/(dab.tool_v26&&press?.37:.29));
      const double plane=dab.has_tool_plane?dab.tool_plane_height:-dab.sign*(dab.tool_v25?dab.tool_depth:.065)*dab.radius;
      double depth=press?penetration_v30b(h[i],plane,r):amount*.30;
      if(dab.stable_v28 && !press)depth=std::min(depth,.08*dab.radius);
      // Reference-based taper is applied below, after the conservative
      // target has been assembled; no independent hidden pressure counter.
      delta[i]=-dab.sign*w[i]*inner*std::min(depth,amount*(dab.tool_v26&&press?.75:.55));
      // Broaden the receiving band while tapering its outer boundary.
      rim[i]=dab.contact_v27b?w[i]*smooth((rr-.22)/.40)*(1-smooth((rr-.78)/.22)):
          w[i]*smooth((r-(dab.tool_v26&&press?.38:.50))/.25);
      if(dab.stable_v28){
        // Feather the outer return profile. First-contact capacity is
        // enforced separately on final targets before budget balancing.
        if(compact)rim[i]*=1-smooth((rr-.65)/.35);
        // Avoid a narrow high receiving band even on an asymmetric mask.
        rim[i]*=1-smooth((rr-.75)/.25);
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

/* V31D2: select a single bounded real-topology patch BEFORE applying
 * averaged orientation. The input weights already reject nonpositive
 * signed support, invalid triangles, mask/hidden and outside-footprint
 * vertices. No graph edge can bridge a zero-factor vertex. Normals are
 * averaged on this patch at two graph scales, not used as motion axes.
 * Distances and final safety are evaluated on the actual geometry. */
inline void connected_contact_v31d2(const std::vector<Vec3> &positions,
                                    const std::vector<Vec3> &dual,
                                    const ClaySurfaceEdges &edges,Vec3 axis,
                                    std::size_t seed,double initial,double reach,double radius,
                                    double blend,std::vector<double> &w,
                                    std::size_t &rejected)
{
  const auto norm=[](Vec3 a){return std::hypot(a.x,a.y,a.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto smooth=[](double v){v=std::clamp(v,0.0,1.0);return v*v*(3-2*v);};
  const auto n=w.size();
  std::vector<std::vector<std::pair<std::size_t,double>>> adj(n);
  for(const auto &e:edges){
    const auto a=e[0],b=e[1];
    if(w[a]<=0 || w[b]<=0)continue;
    const double len=norm(positions[a]-positions[b]);
    if(len<=1e-12 || len>=reach)continue;
    adj[a].push_back({b,len});adj[b].push_back({a,len});
  }
  using Entry=std::pair<double,std::size_t>;
  std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
  std::vector<double> dist(n,std::numeric_limits<double>::infinity()),area(n);
  if(seed<n && w[seed]>0 && initial<reach){
    dist[seed]=initial;queue.push({initial,seed});
  }
  while(!queue.empty()){
    const auto [d,i]=queue.top();queue.pop();
    if(d!=dist[i])continue;
    for(const auto &e:adj[i]){
      const double next=d+e.second;
      if(next<reach && next<dist[e.first]){
        dist[e.first]=next;queue.push({next,e.first});
      }
    }
  }
  std::vector<Vec3> normal(n),filtered(n),next(n),medium(n);
  for(std::size_t i=0;i<n;++i)if(w[i]>0){
    if(!std::isfinite(dist[i])){w[i]=0;++rejected;continue;}
    area[i]=norm(dual[i]);
    if(area[i]<=1e-24){w[i]=0;++rejected;continue;}
    normal[i]=dual[i]/area[i];filtered[i]=normal[i];
  }
  for(int pass=0;pass<4;++pass){
    for(std::size_t i=0;i<n;++i)if(w[i]>0){
      Vec3 sum=filtered[i]*area[i];double mass=area[i];
      for(const auto &e:adj[i])if(w[e.first]>0){
        const double weight=area[e.first]*w[e.first]/
            (1+e.second/std::max(1e-12,.15*radius));
        sum+=filtered[e.first]*weight;mass+=weight;
      }
      const double l=norm(sum);
      next[i]=mass>0 && l>1e-24?sum/l:normal[i];
    }
    filtered.swap(next);
    if(pass==1)medium=filtered;
  }
  for(std::size_t i=0;i<n;++i)if(w[i]>0){
    const double averaged=.5*(dot(medium[i],axis)+dot(filtered[i],axis));
    const double facing=(1-blend)*dot(normal[i],axis)+blend*averaged;
    w[i]*=(1-smooth((dist[i]-(reach-.25*radius))/(.25*radius)))*
        (.15+.85*smooth(facing/.30));
  }
}
inline bool adaptive_direct_v31(const ClaySurfaceDab &dab)
{
  return dab.adaptive_v31 && (dab.action==ClaySurfaceAction::Press ||
      dab.action==ClaySurfaceAction::Scrape || dab.action==ClaySurfaceAction::Compact ||
      dab.action==ClaySurfaceAction::Compress);
}

/* V31A: direct contact on a bounded edge-geodesic patch. The native
 * footprint and artist masks remain authoritative. Current incident
 * triangles define oriented dual areas and topology. All accepted targets
 * move along the single current tool axis; local normals select, not deform.
 * Never bridge zero-influence vertices or disconnected sheets. This is
 * first-order volume accounting for nonparallel motions, not exact CCD
 * or a multi-chart advective solver. */
inline ClaySurfaceResult solve_adaptive_contact_v31(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab,ClaySurfaceEdges *dab_edges)
{
  const auto norm=[](Vec3 a){return std::hypot(a.x,a.y,a.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto smooth=[](double x){x=std::clamp(x,0.0,1.0);return x*x*(3-2*x);};
  const std::size_t n=vertices.size();
  ClaySurfaceResult out;
  out.displacement.resize(n);out.projected_area.assign(n,0.0);
  std::vector<Vec3> positions(n),dual(n),normals(n);
  std::vector<double> w(n,0),radial(n),distance(n,std::numeric_limits<double>::infinity());
  std::vector<bool> bad(n,false);
  const Vec3 axis=dab.axis/norm(dab.axis);
  ClaySurfaceEdges local_edges;
  auto &edges=dab_edges?*dab_edges:local_edges;
  const bool build=edges.empty();
  for(std::size_t i=0;i<n;++i){
    positions[i]=vertices[i].position;
    if(!std::isfinite(norm(positions[i])) || !std::isfinite(vertices[i].influence) ||
       vertices[i].influence<0 || vertices[i].influence>1 ||
       (dab.plastic_v29 && (!std::isfinite(vertices[i].compaction_v29) ||
        vertices[i].compaction_v29<0 || vertices[i].compaction_v29>1)))
      throw std::invalid_argument("Invalid V31 contact vertex");
  }
  for(const auto &tri:triangles){
    for(auto i:tri)if(i>=n)throw std::out_of_range("V31 triangle index");
    const Vec3 a=cross(positions[tri[1]]-positions[tri[0]],positions[tri[2]]-positions[tri[0]])/6.0;
    const double area=norm(a);
    if(!std::isfinite(area))throw std::overflow_error("V31 dual area");
    for(auto i:tri){dual[i]+=a;if(area<=1e-24)bad[i]=true;}
    if(build)for(int j=0;j<3;++j){
      auto a=tri[j],b=tri[(j+1)%3];if(a>b)std::swap(a,b);
      if(a!=b)edges.push_back({a,b});
    }
  }
  if(build){std::sort(edges.begin(),edges.end());edges.erase(std::unique(edges.begin(),edges.end()),edges.end());}
  std::size_t seed=n;
  double nearest=std::numeric_limits<double>::infinity();
  for(std::size_t i=0;i<n;++i){
    const double area=norm(dual[i]);
    out.projected_area[i]=std::max(0.0,dot(dual[i],axis));
    if(vertices[i].influence<=0)continue;
    ++out.adaptive_candidates_v31;
    if(bad[i] || area<=1e-24){++out.chart_rejected_v30b;continue;}
    normals[i]=dual[i]/area;
    // Steep front-facing slopes remain valid. Reject the back of a thin
    // sheet and disallow sharp orientation jumps along the graph.
    if(dot(normals[i],axis)<=.015){++out.chart_rejected_v30b;continue;}
    const Vec3 off=positions[i]-dab.center;
    radial[i]=chart_distance_v29(off-axis*dot(off,axis),dab,axis)/dab.radius;
    if(radial[i]>=1)continue;
    w[i]=vertices[i].influence;
    const double dist=norm(off);
    if(dist<nearest){nearest=dist;seed=i;}
  }
  if(seed==n || dab.dose==0 || dab.intensity==0)return out;
  const double segment=dab.swept_v29?norm(dab.sweep_start-dab.center):0.0;
  const double reach=1.5*dab.radius+segment;
  if(nearest>=reach)return out;
  const bool continuous=dab.continuous_contact_v31d2 && dab.average_surface_v31d>0;
  if(continuous){
    connected_contact_v31d2(positions,dual,edges,axis,seed,nearest,reach,dab.radius,
                             dab.average_surface_v31d,w,out.component_rejected_v30b);
  }else{
  std::vector<std::vector<std::pair<std::size_t,double>>> adj(n);
  for(const auto &e:edges){
    const auto a=e[0],b=e[1];
    if(w[a]<=0 || w[b]<=0 || dot(normals[a],normals[b])<.20)continue;
    const double len=norm(positions[a]-positions[b]);
    if(len<=1e-12 || len>reach)continue;
    adj[a].push_back({b,len});adj[b].push_back({a,len});
  }
  using Entry=std::pair<double,std::size_t>;
  std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
  distance[seed]=nearest;queue.push({nearest,seed});
  while(!queue.empty()){
    const auto [dist,i]=queue.top();queue.pop();
    if(dist!=distance[i])continue;
    for(const auto &edge:adj[i]){
      const double next=dist+edge.second;
      if(next<reach && next<distance[edge.first]){
        distance[edge.first]=next;queue.push({next,edge.first});
      }
    }
  }
  }
  double max_r=0;
  for(std::size_t i=0;i<n;++i)if(w[i]>0){
    if(!continuous){
    if(!std::isfinite(distance[i])){w[i]=0;++out.component_rejected_v30b;continue;}
    w[i]*=1-smooth((distance[i]-(reach-.25*dab.radius))/(.25*dab.radius));
    // Coherent axial motion, with a smooth reduction on nearly vertical
    // surfaces; no independent vertex-normal displacement or depth slab.
    w[i]*=smooth(dot(normals[i],axis)/.30);
    }
    if(w[i]<=0)continue;
    ++out.adaptive_reached_v31;
    if(std::abs(dot(positions[i]-dab.center,axis))>dab.radius*(dab.contact_v30b?.65:.50))
      ++out.adaptive_deep_v31;
    if(w[i]>.02)max_r=std::max(max_r,radial[i]);
  }
  out.active=out.adaptive_reached_v31;
  out.patches_v30c=out.active?1:0;
  if(!out.active)return out;
  const bool scrape=dab.action==ClaySurfaceAction::Scrape;
  const bool press=dab.action==ClaySurfaceAction::Press;
  const bool compact=dab.action==ClaySurfaceAction::Compact;
  const double amount=std::min(.08*dab.radius,dab.radius*dab.dose*dab.intensity*
      (.35+1.65*dab.mobility)*(1-.45*dab.stiffness)*(1-.30*dab.cohesion));
  const double rim_radius=std::clamp(max_r/.88,.25,1.0);
  std::vector<double> reference;
  if(dab.average_surface_v31d>0 && (press||scrape)){
    std::vector<Vec3> xy(n);std::vector<double> height(n);
    for(std::size_t i=0;i<n;++i){
      const Vec3 off=positions[i]-dab.center;
      height[i]=dot(off,axis);xy[i]=off-axis*height[i];
    }
    if(average_reference_v31d(xy,height,w,out.projected_area,axis,dab.radius,reference,continuous)){
      ++out.average_fits_v31d;
      for(std::size_t i=0;i<n;++i)if(w[i]>0)
        out.average_peak_v31d=std::max(out.average_peak_v31d,std::abs(reference[i]-height[i])/dab.radius);
    }else reference.clear();
  }
  std::vector<double> delta(n),rim(n);
  double removed=0,receiver=0,peak_rim=0;
  for(std::size_t i=0;i<n;++i)if(w[i]>0){
    const double r=radial[i],rr=r/rim_radius;
    const double inner=1-smooth((rr-.30)/.49);
    // A bounded local indentation/cut is evaluated on the CURRENT point.
    // This intentionally differs from the old single-plane cutter.
    const double actual_height=dot(positions[i]-dab.center,axis);
    const double contact_height=reference.empty()?actual_height:
        actual_height+(reference[i]-actual_height)*dab.average_surface_v31d;
    const double nominal=std::max(0.0,dab.sign*(contact_height-
                        (dab.has_tool_plane?dab.tool_plane_height:-dab.sign*dab.tool_depth*dab.radius)));
    const double local_depth=std::max(nominal,.35*dab.tool_depth*dab.radius);
    const double cut=scrape?std::min(local_depth,amount*.85):
                     press?inner*std::min(local_depth,amount*.75):
                     inner*amount*(compact?.24:.30);
    delta[i]=-dab.sign*w[i]*cut;
    if(dab.plastic_v29 && dab.sign>0 && !scrape){
      const double hardening=compact?.85:press?.45:.60;
      delta[i]*=1-hardening*vertices[i].compaction_v29;
    }
    removed+=std::abs(delta[i])*out.projected_area[i];
    if(!scrape){
      rim[i]=w[i]*smooth((rr-.22)/.40)*(1-smooth((rr-.78)/.22));
      receiver+=rim[i]*out.projected_area[i];peak_rim=std::max(peak_rim,rim[i]);
    }
  }
  if(!scrape){
    const double fraction=compact?std::min(.40,dab.volume_preservation):1.0;
    const double coefficient=receiver>1e-24?fraction*removed/receiver:0;
    const double peak=coefficient*peak_rim;
    const double accepted=receiver<=1e-24?0:peak>amount*.30?amount*.30/peak:1;
    for(std::size_t i=0;i<n;++i)
      delta[i]=delta[i]*((1-fraction)+fraction*accepted)+dab.sign*accepted*coefficient*rim[i];
  }
  // First-order signed budget using actual oriented dual vectors.
  double pos=0,neg=0;
  for(std::size_t i=0;i<n;++i){
    const double v=delta[i]*out.projected_area[i];
    pos+=std::max(0.0,v);neg+=std::max(0.0,-v);
    // A noisy vertex normal must never create a divergent displacement.
    // Keep the tool direction coherent across the entire current dab.
    out.displacement[i]=axis*delta[i];
  }
  // Bound differential motion BEFORE the triangle guard. Uniform scaling
  // retains both the smooth profile and the signed budget; no individually
  // stalled vertices injected into a pressure field by the local limiter.
  const double coherent=dab.gradient_v31c?
      gradient_motion_scale_v31c(positions,triangles,out.displacement):
      coherent_motion_scale_v31b(positions,triangles,out.displacement);
  if(coherent<1)for(auto &v:out.displacement)v=v*coherent;
  const auto safe=limit_local_contact(positions,triangles,out.displacement,true);
  out.slope_scale=coherent;
  out.accepted_scale=coherent*safe.final_scale;
  out.local_limited_v30b=safe.limited_vertices;
  pos=0;neg=0;
  for(std::size_t i=0;i<n;++i){
    const double v=dot(dual[i],out.displacement[i]);
    pos+=std::max(0.0,v);neg+=std::max(0.0,-v);
  }
  out.signed_swept_volume=pos-neg;out.transported_volume=std::min(pos,neg);
  return out;
}

// Optional per-dab topology cache, populated only from validated triangles.
// Callers must clear it when changing the local vertex/triangle indexing.
inline ClaySurfaceResult solve_clay_surface(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab,
    ClaySurfaceEdges *dab_edges=nullptr,bool allow_patch_split=true)
{
  const auto norm=[](Vec3 a){return std::hypot(a.x,a.y,a.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{
      a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto interval=[](double x){return std::isfinite(x)&&x>=0&&x<=1;};
  if (!std::isfinite(norm(dab.center)) || !std::isfinite(norm(dab.axis)) ||
      norm(dab.axis)<=1e-12 || !std::isfinite(norm(dab.direction)) ||
      !std::isfinite(dab.radius) || dab.radius<=1e-12 ||
      (dab.swept_v29 && (!std::isfinite(norm(dab.sweep_start)) || norm(dab.sweep_start-dab.center)>dab.radius*.500001)) ||
      !interval(dab.dose) || !interval(dab.transport_load_v30b) || !interval(dab.average_surface_v31d) ||
      !std::isfinite(dab.push_gain_v30d) || dab.push_gain_v30d<.5 || dab.push_gain_v30d>4.0 || !std::isfinite(dab.intensity) || dab.intensity<0 ||
      dab.intensity>(dab.shape_transport?4.0:1.0) || !interval(dab.mobility) ||
      (dab.sign!=1 && dab.sign!=-1) ||
      !interval(dab.yield_slope) || !interval(dab.relative_slope_limit) ||
      dab.relative_slope_limit==0 ||
      !interval(dab.friction) || !interval(dab.viscosity) ||
      !interval(dab.cohesion) || !interval(dab.stiffness) ||
      !interval(dab.plasticity) || !interval(dab.volume_preservation) ||
      !interval(dab.fusion) || !interval(dab.fold_gain) || !interval(dab.detail_retention) ||
      !interval(dab.tool_depth) || !interval(dab.melt_local_gravity_v27b) || !std::isfinite(dab.tool_plane_height) ||
      (dab.refine_v28b&&!dab.stable_v28) || (dab.tool_v26&&!dab.tool_v25) || (dab.tool_v25&&!dab.shape_transport) || dab.proxy_cells_per_radius<4 || dab.proxy_cells_per_radius>16) throw std::invalid_argument("Invalid clay surface dab");
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
  if(adaptive_direct_v31(dab))return solve_adaptive_contact_v31(vertices,triangles,dab,dab_edges);
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
  const bool recover_contact=dab.contact_v30b && (dab.action==ClaySurfaceAction::Press ||
      dab.action==ClaySurfaceAction::Scrape || dab.action==ClaySurfaceAction::Compact ||
      dab.action==ClaySurfaceAction::Compress);
  const bool average=average_transport_v31d(dab);
  const bool continuous=average && dab.continuous_contact_v31d2;
  std::vector<double> support_area((recover_contact||continuous)?n:0,0.0);
  std::vector<bool> mixed((recover_contact||continuous)?n:0,false);
  std::vector<Vec3> dual(continuous?n:0);
  const double depth_limit=recover_contact?.65:.50;
  const double spatial_limit=1.5*dab.radius+(dab.swept_v29?norm(dab.sweep_start-dab.center):0.0);
  for(std::size_t i=0;i<n;++i) {
    if(!std::isfinite(norm(vertices[i].position)) || !interval(vertices[i].influence) ||
       (dab.plastic_v29 && !interval(vertices[i].compaction_v29)) ||
       (dab.stable_v28 && (!std::isfinite(vertices[i].contact_radius) || vertices[i].contact_radius<0 ||
                          !std::isfinite(norm(vertices[i].contact_reference)))))
      throw std::invalid_argument("Invalid clay surface vertex");
    const Vec3 offset=vertices[i].position-dab.center;
    h[i]=dot(offset,axis);
    xy[i]=tangent(offset);
    if(!std::isfinite(h[i]) || !std::isfinite(norm(xy[i])))
      throw std::overflow_error("Invalid clay chart");
    // Explicit depth bound avoids gathering the back side of a thin mesh.
    if(!average && vertices[i].influence>0 && std::abs(h[i])>dab.radius*depth_limit)++out.depth_rejected_v30b;
    if((average?norm(offset)<spatial_limit:std::abs(h[i])<=dab.radius*depth_limit) &&
       ((dab.surface_contact && !dab.shape_transport) || chart_distance_v29(xy[i],dab,axis)<=dab.radius)) {
      w[i]=vertices[i].influence;
      if(dab.shape_transport && !average) {
        const double t=std::clamp((depth_limit-std::abs(h[i])/dab.radius)/.15,0.0,1.0);
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
    if(recover_contact||continuous){
      // Signed dual support from every incident triangle; no remapping here.
      for(auto i:t){
        if(area<=1e-24)bad[i]=true;
        if(projected<=.10*area)mixed[i]=true;
        support_area[i]+=area/3.0;
        out.projected_area[i]+=projected/3.0;
        if(continuous)dual[i]+=av/3.0;
      }
    }else if(area<=1e-24 || projected<=0.10*area) {
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
    if(continuous){
      // Keep actual positive signed support; a single steep incident face
      // no longer rejects an otherwise usable vertex. Never invent area.
      const double support=support_area[i]>1e-24?out.projected_area[i]/support_area[i]:0.0;
      if(support<=.015)bad[i]=true;
      else if(w[i]>0 && !bad[i] && mixed[i])++out.chart_recovered_v30b;
    }else if(recover_contact){
      const double support=support_area[i]>1e-24?out.projected_area[i]/support_area[i]:0.0;
      if(support<=.06)bad[i]=true;
      else if(w[i]>0 && !bad[i] && mixed[i]){
        ++out.chart_recovered_v30b;
        const double a=std::clamp((support-.06)/.24,0.0,1.0);
        w[i]*=a*a*(3-2*a);
      }
    }
    if(bad[i] || out.projected_area[i]<=1e-24){
      if(w[i]>0)++out.chart_rejected_v30b;
      w[i]=0;
      if(out.projected_area[i]<0)out.projected_area[i]=0;
    }
    if(w[i]>0) {
      const double d=norm(vertices[i].position-dab.center);
      if(d<nearest){nearest=d;seed=i;}
    }
  }
  if(seed==n || dab.dose==0 || dab.intensity==0) return out;
  if(continuous){
    std::vector<Vec3> positions(n);
    for(std::size_t i=0;i<n;++i)positions[i]=vertices[i].position;
    connected_contact_v31d2(positions,dual,edges,axis,seed,nearest,spatial_limit,dab.radius,
                             dab.average_surface_v31d,w,out.component_rejected_v30b);
    std::vector<double> reference;
    const bool fitted=average_reference_v31d(xy,h,w,out.projected_area,axis,dab.radius,reference,true);
    if(fitted)++out.average_fits_v31d;
    // A depth residual modulates pressure, rather than splitting a
    // connected supported sheet into arbitrary axial-slab islands.
    // The real geodesic bound, positive support and mask remain hard.
    for(std::size_t i=0;i<n;++i)if(w[i]>0){
      const double depth=std::abs(h[i]-(fitted?dab.average_surface_v31d*reference[i]:0.0));
      if(fitted)out.average_peak_v31d=std::max(out.average_peak_v31d,std::abs(reference[i]-h[i])/dab.radius);
      if(std::abs(h[i])>depth_limit*dab.radius)++out.average_recovered_v31d;
      const double f=std::clamp((.85-depth/dab.radius)/.35,0.0,1.0);
      w[i]*=.15+.85*f*f*(3-2*f);
    }
    if(w[seed]<=0)return out;
    out.patches_v30c=1;
  }else if(average){
    // Fit only the pre-depth, positive-factor, front-facing component
    // nearest the cursor. Existing chart rejects and real topology remain
    // authoritative. Other islands retain their original depth envelope.
    std::vector<std::size_t> parent(n);
    for(std::size_t i=0;i<n;++i)parent[i]=i;
    const auto root=[&](std::size_t i){
      while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;
    };
    for(const auto &e:edges)if(w[e[0]]>0 && w[e[1]]>0){
      const auto a=root(e[0]),b=root(e[1]);if(a!=b)parent[b]=a;
    }
    const auto component=root(seed);
    auto weights=w;
    for(std::size_t i=0;i<n;++i)if(w[i]>0 && root(i)!=component)weights[i]=0;
    std::vector<double> reference;
    const bool fitted=average_reference_v31d(xy,h,weights,out.projected_area,axis,dab.radius,reference);
    if(fitted)++out.average_fits_v31d;
    seed=n;nearest=std::numeric_limits<double>::infinity();
    for(std::size_t i=0;i<n;++i)if(w[i]>0){
      const bool supported=fitted && weights[i]>0;
      const double depth=std::abs(h[i]-(supported?dab.average_surface_v31d*reference[i]:0.0));
      if(supported)out.average_peak_v31d=std::max(out.average_peak_v31d,std::abs(reference[i]-h[i])/dab.radius);
      if(depth>=depth_limit*dab.radius){w[i]=0;++out.depth_rejected_v30b;continue;}
      if(std::abs(h[i])>depth_limit*dab.radius)++out.average_recovered_v31d;
      const double feather=std::clamp((depth_limit-depth/dab.radius)/.15,0.0,1.0);
      w[i]*=feather*feather*(3-2*feather);
      const double dist=norm(vertices[i].position-dab.center);
      if(w[i]>0 && dist<nearest){nearest=dist;seed=i;}
    }
    if(seed==n)return out;
  }
  // V30C: separate topological support from writable transport components.
  // The halo includes incident faces, including zero-factor vertices.
  // Other disconnected mesh sheets are still excluded. Within the touched
  // sheet, solve each writable island independently so no proxy flux crosses
  // a mask or an excluded strip of steep vertices. Do not reuse any geometry
  // or goals from an earlier dab.
  if(dab.overlap_v30c && allow_patch_split && !continuous){
    std::vector<std::size_t> sheet(n),active_parent(n);
    for(std::size_t i=0;i<n;++i)sheet[i]=active_parent[i]=i;
    const auto root=[](std::vector<std::size_t> &p,std::size_t i){
      while(p[i]!=i){p[i]=p[p[i]];i=p[i];}return i;
    };
    const auto unite=[&](std::vector<std::size_t> &p,std::size_t a,std::size_t b){
      a=root(p,a);b=root(p,b);if(a!=b)p[b]=a;
    };
    for(const auto &e:edges){
      unite(sheet,e[0],e[1]);
      if(w[e[0]]>0 && w[e[1]]>0)unite(active_parent,e[0],e[1]);
    }
    struct Patch {std::vector<std::size_t> ids;double nearest=std::numeric_limits<double>::infinity();};
    std::map<std::size_t,Patch> patches;
    const auto main_sheet=root(sheet,seed);
    for(std::size_t i=0;i<n;++i)if(w[i]>0){
      if(root(sheet,i)!=main_sheet){++out.component_rejected_v30b;continue;}
      auto &patch=patches[root(active_parent,i)];
      patch.ids.push_back(i);
      patch.nearest=std::min(patch.nearest,norm(vertices[i].position-dab.center));
    }
    if(patches.size()>1){
      std::vector<const Patch *> ordered;
      for(const auto &entry:patches)ordered.push_back(&entry.second);
      std::stable_sort(ordered.begin(),ordered.end(),[](const Patch *a,const Patch *b){
        return a->nearest<b->nearest;
      });
      // Bound worst-case work on fragmented masks; report every skipped
      // eligible vertex rather than pretending that it was processed.
      constexpr std::size_t max_patches=8;
      auto local_vertices=vertices;
      for(auto &v:local_vertices)v.influence=0.0;
      for(const Patch *patch:ordered){
        if(out.patches_v30c>=max_patches){out.patch_budget_skipped_v30c+=patch->ids.size();continue;}
        for(auto i:patch->ids)local_vertices[i].influence=vertices[i].influence;
        const auto part=solve_clay_surface(local_vertices,triangles,dab,&edges,false);
        ++out.patches_v30c;
        out.active+=part.active;out.transport_nodes+=part.transport_nodes;
        out.trace_attempts+=part.trace_attempts;out.trace_rejected+=part.trace_rejected;
        out.history_limited+=part.history_limited;out.local_limited_v30b+=part.local_limited_v30b;
        out.slope_scale=std::min(out.slope_scale,part.slope_scale);
        out.accepted_scale=std::min(out.accepted_scale,part.accepted_scale);
        out.signed_swept_volume+=part.signed_swept_volume;
        out.transported_volume+=part.transported_volume;
        out.fine_transport_v30b+=part.fine_transport_v30b;
        out.melt_fit_failures_v30b+=part.melt_fit_failures_v30b;
        out.melt_affine_fallbacks_v31c+=part.melt_affine_fallbacks_v31c;
        // Parent already counted pre-split candidate recovery; report
        // the largest residual and avoid double-counting the same ids.
        out.average_peak_v31d=std::max(out.average_peak_v31d,part.average_peak_v31d);
        out.melt_residual_peak_v30b=std::max(out.melt_residual_peak_v30b,part.melt_residual_peak_v30b);
        for(auto i:patch->ids){
          out.displacement[i]=part.displacement[i];
          local_vertices[i].influence=0.0;
        }
      }
      return out;
    }
    // Single component uses original fast path. Avoid double counting its
    // excluded sheets in the ordinary component accounting below.
    out.component_rejected_v30b=0;
    out.patches_v30c=1;
  }
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
    if(!seen[i]){if(w[i]>0)++out.component_rejected_v30b;w[i]=0;}
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
                          mobility,yield,drag_gain,pressure_gain,delta,out,&edges);
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
    if(!dab.live_surface_v30)for(std::size_t i=0;i<n;++i)if(delta[i]!=0 && vertices[i].contact_radius>0){
      const double limit=history_radius_fraction_v28(dab.action)*vertices[i].contact_radius;
      const Vec3 offset=vertices[i].position-vertices[i].contact_reference;
      double scale=history_scale_v28(vertices[i].position,vertices[i].contact_reference,axis*delta[i],limit);
      if(dab.refine_v28b && dot(offset,axis*delta[i])>0){
        // Smoothly reduce only outward-from-reference motion as a point
        // approaches saturation. Recovery stays fully available.
        const double t=std::clamp((limit-norm(offset))/(.45*limit),0.0,1.0);
        scale=std::min(scale,t*t*(3-2*t));
      }
      if(scale<1)++out.history_limited;
      delta[i]*=scale;
    }
    if(dab.plastic_v29 && dab.sign>0 &&
       (dab.action==ClaySurfaceAction::Press || dab.action==ClaySurfaceAction::Compress ||
        dab.action==ClaySurfaceAction::Compact)){
      const double hardening=dab.action==ClaySurfaceAction::Compact?.85:
                             dab.action==ClaySurfaceAction::Compress?.60:.45;
      for(std::size_t i=0;i<n;++i)if(delta[i]<0)
        delta[i]*=1-hardening*vertices[i].compaction_v29;
    }
    if(recover_contact && dab.live_surface_v30){
      // Local reductions before signed budget rebalance and final check.
      for(int pass=0;pass<3;++pass){
        bool changed=false;
        for(const auto &e:edges){
          const auto a=e[0],b=e[1];
          const double diff=std::abs(delta[a]-delta[b]);
          const double limit=dab.relative_slope_limit*norm(vertices[a].position-vertices[b].position);
          if(diff<=limit || diff<=1e-24)continue;
          const double scale=limit/diff;
          delta[a]*=scale;delta[b]*=scale;
          ++out.local_limited_v30b;changed=true;
        }
        if(!changed)break;
      }
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
    const double len=dab.live_surface_v30?norm(vertices[e[0]].position-vertices[e[1]].position):
                                         norm(xy[e[0]]-xy[e[1]]);
    if(diff>0) {
      const double existing=h[e[0]]-h[e[1]];
      // Allow reduction of existing slope without treating it as a new
      // instability. Still bound any increase of absolute height slope.
      const double recovery=dab.surface_contact && existing*increment<0 ?
          std::abs(existing):0.0;
      // V30 accepts a bounded increment on the CURRENT 3D edge rather than
      // permanently freezing a steep but valid edge against an absolute slope.
      // Native triangle contact validation still runs on the accepted batch.
      const double scale=dab.live_surface_v30?dab.relative_slope_limit*len/diff:guarded?
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
  if(continuous){
    // Reuse the V31 coherent guard on the real triangles, including halo.
    std::vector<Vec3> positions(n);
    for(std::size_t i=0;i<n;++i)positions[i]=vertices[i].position;
    const double scale=dab.gradient_v31c?gradient_motion_scale_v31c(positions,triangles,out.displacement):
        coherent_motion_scale_v31b(positions,triangles,out.displacement);
    if(scale<1)for(auto &v:out.displacement)v=v*scale;
    const auto safe=limit_local_contact(positions,triangles,out.displacement,true);
    out.accepted_scale*=scale*safe.final_scale;
    out.slope_scale=std::min(out.slope_scale,scale);
    out.signed_swept_volume*=scale*safe.final_scale;
  }
  out.transported_volume*=out.accepted_scale;
  out.fine_transport_v30b*=out.accepted_scale;
  if(!std::isfinite(out.signed_swept_volume)||!std::isfinite(out.transported_volume))
    throw std::overflow_error("Invalid clay surface volume");
  return out;
}

} // namespace sculpt_physics
