/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "clay_surface_solver.hh"

namespace sculpt_physics {
struct TangentialTransportV31D6 {
  std::size_t moved=0;
  double scale=0, peak=0, signed_volume=0;
};

/* Add a bounded material-following surface translation to the accepted
 * V31 height field. Tangency to the actual oriented dual area guarantees
 * zero additional FIRST-ORDER swept volume, not finite-step mass or UV
 * conservation. Never add targets outside the selected contact. */
inline TangentialTransportV31D6 add_tangential_transport_v31d6(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab,const std::vector<double> &weights,
    std::vector<Vec3> &motion)
{
  TangentialTransportV31D6 out;
  if(dab.tangential_gain_v31d6<=0 || dab.dose<=0 || dab.intensity<=0 ||
     (dab.action!=ClaySurfaceAction::Push && dab.action!=ClaySurfaceAction::Smear))
    return out;
  const auto n=vertices.size();
  if(weights.size()!=n || motion.size()!=n)return out;
  const auto norm=[](Vec3 p){return std::hypot(p.x,p.y,p.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  if(!std::isfinite(dab.tangential_gain_v31d6) || dab.tangential_gain_v31d6>.5 ||
     !std::isfinite(dab.radius) || dab.radius<=0 || !std::isfinite(dab.dose) ||
     !std::isfinite(dab.intensity) || !std::isfinite(norm(dab.direction)))return out;
  const double dn=norm(dab.direction);
  if(dn<=1e-12)return out;
  const Vec3 direction=dab.direction*(dab.sign/dn);
  std::vector<Vec3> p(n),dual(n),extra(n),trial(n);
  std::vector<double> min_edge(n,std::numeric_limits<double>::infinity());
  for(std::size_t i=0;i<n;++i){
    p[i]=vertices[i].position;
    if(!std::isfinite(norm(p[i])) || !std::isfinite(norm(motion[i])) ||
       !std::isfinite(weights[i]) || weights[i]<0 || weights[i]>1)return out;
  }
  for(const auto &t:triangles){
    for(auto i:t)if(i>=n)throw std::out_of_range("D6 triangle index");
    const Vec3 a=cross(p[t[1]]-p[t[0]],p[t[2]]-p[t[0]])/6.;
    for(auto i:t)dual[i]+=a;
    for(int j=0;j<3;++j){
      const auto i=t[j],k=t[(j+1)%3];
      const double len=norm(p[i]-p[k]);
      min_edge[i]=std::min(min_edge[i],len);
      min_edge[k]=std::min(min_edge[k],len);
    }
  }
  const double amplitude=std::min(.04*dab.radius,
      dab.radius*dab.dose*dab.intensity*dab.tangential_gain_v31d6*
      (.25+.75*dab.plasticity)/(1+2*dab.viscosity));
  std::size_t candidates=0;
  for(std::size_t i=0;i<n;++i){
    if(weights[i]<=0 || vertices[i].influence<=0 || !std::isfinite(min_edge[i]))continue;
    const double a=norm(dual[i]);if(a<=1e-24)continue;
    const Vec3 normal=dual[i]/a;
    Vec3 t=direction-normal*dot(direction,normal);
    const double len=norm(t);
    if(len<=1e-12)continue;
    const double step=std::min(amplitude*std::min(weights[i],vertices[i].influence),
                               .10*min_edge[i]);
    if(step<=1e-15)continue;
    extra[i]=t*step;++candidates;
  }
  if(!candidates)return out;
  // Keep the existing V31 coherent guard. Backtrack the OPTIONAL tangent
  // term if it would materially reduce the baseline acceptance.
  const auto acceptance=[&](const std::vector<Vec3> &m){
    double scale=dab.gradient_v31c?gradient_motion_scale_v31c(p,triangles,m):
                                   coherent_motion_scale_v31b(p,triangles,m);
    for(const auto &t:triangles){
      ContactTriangle tri{};
      for(int j=0;j<3;++j){tri.position[j]=p[t[j]];tri.motion[j]=m[t[j]];}
      scale=std::min(scale,contact_triangle_scale(tri));
    }
    return scale;
  };
  const double baseline=acceptance(motion);
  if(baseline<=0)return out;
  for(double s=1.;s>=1./128.;s*=.5){
    for(std::size_t i=0;i<n;++i)trial[i]=motion[i]+extra[i]*s;
    if(acceptance(trial)+1e-12<baseline*.98)continue;
    motion.swap(trial);
    out.scale=s;
    for(std::size_t i=0;i<n;++i){
      const Vec3 d=extra[i]*s;
      const double len=norm(d);
      if(len>0)++out.moved;
      out.peak=std::max(out.peak,len);
      out.signed_volume+=dot(dual[i],d);
    }
    break;
  }
  return out;
}
} // namespace sculpt_physics
