/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "clay_surface_solver.hh"

namespace sculpt_physics {

/* Integrates a single native dab in bounded material substeps. The host
 * evaluates its exact mask, texture and footprint once per native dab.
 * Not continuous collision detection: no vertices outside that footprint
 * are added, and the material chart stays fixed during this integration.
 * Geometry itself is the accepted relief history; no hidden mesh state. */
struct MaterialTransportV21Result {
  ClaySurfaceResult surface;
  int substeps=0;
  std::size_t refinement_edges=0;
};

inline MaterialTransportV21Result solve_material_transport_v21(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    ClaySurfaceDab dab,
    int substeps=4)
{
  if(substeps<1 || substeps>16 || !std::isfinite(dab.dose) ||
     dab.dose<0 || dab.dose>1)
    throw std::invalid_argument("Invalid V21 integration step");
  dab.extended_material=true;
  dab.surface_contact=true;
  // Divide the total requested dose, not the strength. Reevaluate
  // accepted local geometry and tool potential between increments.
  dab.dose/=double(substeps);
  auto work=vertices;
  MaterialTransportV21Result out;
  out.surface.displacement.resize(vertices.size());
  for(int k=0;k<substeps;++k) {
    const auto step=solve_clay_surface(work,triangles,dab);
    if(k==0) out.surface.projected_area=step.projected_area;
    out.surface.active=std::max(out.surface.active,step.active);
    out.surface.transport_nodes=std::max(out.surface.transport_nodes,step.transport_nodes);
    out.surface.accepted_scale=std::min(out.surface.accepted_scale,step.accepted_scale);
    out.surface.transported_volume+=step.transported_volume;
    out.surface.signed_swept_volume+=step.signed_swept_volume;
    for(std::size_t i=0;i<work.size();++i) {
      work[i].position+=step.displacement[i];
      out.surface.displacement[i]+=step.displacement[i];
    }
    ++out.substeps;
  }
  // Diagnose local resolution demand, not a native topology operation.
  // Count unique long or steep edges with at least one active endpoint.
  std::vector<std::array<std::size_t,2>> edges;
  for(const auto &t:triangles)
    for(int j=0;j<3;++j) {
      auto a=t[j],b=t[(j+1)%3];
      if(a>b)std::swap(a,b);
      edges.push_back({a,b});
    }
  std::sort(edges.begin(),edges.end());
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  for(const auto &e:edges) {
    const auto a=e[0],b=e[1];
    if(vertices[a].influence<=0 && vertices[b].influence<=0) continue;
    const double before=norm(vertices[a].position-vertices[b].position);
    const double after=norm(work[a].position-work[b].position);
    if(after>dab.radius*0.20 || (before>1e-12 && after>1.4*before))
      ++out.refinement_edges;
  }
  return out;
}

} // namespace sculpt_physics
