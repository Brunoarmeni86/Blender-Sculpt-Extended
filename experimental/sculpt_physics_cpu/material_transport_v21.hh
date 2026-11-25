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
    int substeps=4,
    bool diagnose_refinement=true,
    bool reuse_topology=true)
{
  if(substeps<1 || substeps>16 || !std::isfinite(dab.dose) ||
     dab.dose<0 || dab.dose>1)
    throw std::invalid_argument("Invalid V21 integration step");
  dab.extended_material=true;
  dab.surface_contact=true;
  // Divide the total requested dose, not the strength. Reevaluate
  // accepted local geometry and tool potential between increments.
  if(dab.smooth_proxy) {
    // Native dabs with small travel need fewer repeated transfers.
    // This is a bounded heuristic, not a complete CFL estimator.
    const double intensity=dab.shape_transport?std::clamp(dab.intensity,0.0,4.0):1.0;
    if(!std::isfinite(intensity)) throw std::invalid_argument("Invalid material intensity");
    substeps=std::clamp(int(std::ceil(dab.dose*intensity*std::clamp(dab.proxy_cells_per_radius,4,16)*4)),1,substeps);
  }
  dab.dose/=double(substeps);
  auto work=vertices;
  MaterialTransportV21Result out;
  out.surface.displacement.resize(vertices.size());
  ClaySurfaceEdges dab_edges;
  const bool cache_topology=(dab.tool_v26 || dab.fast_v26b) && reuse_topology;
  for(int k=0;k<substeps;++k) {
    const auto step=solve_clay_surface(work,triangles,dab,cache_topology?&dab_edges:nullptr);
    if(k==0) out.surface.projected_area=step.projected_area;
    out.surface.active=std::max(out.surface.active,step.active);
    out.surface.transport_nodes=std::max(out.surface.transport_nodes,step.transport_nodes);
    out.surface.trace_attempts+=step.trace_attempts;
    out.surface.trace_rejected+=step.trace_rejected;
    out.surface.history_limited+=step.history_limited;
    out.surface.fine_transport_v30b+=step.fine_transport_v30b;
    out.surface.chart_rejected_v30b+=step.chart_rejected_v30b;
    out.surface.chart_recovered_v30b+=step.chart_recovered_v30b;
    out.surface.depth_rejected_v30b+=step.depth_rejected_v30b;
    out.surface.component_rejected_v30b+=step.component_rejected_v30b;
    out.surface.local_limited_v30b+=step.local_limited_v30b;
    out.surface.adaptive_candidates_v31=std::max(out.surface.adaptive_candidates_v31,step.adaptive_candidates_v31);
    out.surface.adaptive_reached_v31=std::max(out.surface.adaptive_reached_v31,step.adaptive_reached_v31);
    out.surface.adaptive_deep_v31=std::max(out.surface.adaptive_deep_v31,step.adaptive_deep_v31);
    out.surface.patches_v30c=std::max(out.surface.patches_v30c,step.patches_v30c);
    out.surface.patch_budget_skipped_v30c+=step.patch_budget_skipped_v30c;
    out.surface.melt_fit_failures_v30b+=step.melt_fit_failures_v30b;
    out.surface.melt_affine_fallbacks_v31c+=step.melt_affine_fallbacks_v31c;
    out.surface.average_fits_v31d+=step.average_fits_v31d;
    out.surface.average_recovered_v31d=std::max(out.surface.average_recovered_v31d,step.average_recovered_v31d);
    out.surface.average_peak_v31d=std::max(out.surface.average_peak_v31d,step.average_peak_v31d);
    out.surface.proxy_passes_v31d3+=step.proxy_passes_v31d3;
    out.surface.proxy_rejected_v31d3+=step.proxy_rejected_v31d3;
    out.surface.proxy_nodes_v31d3=std::max(out.surface.proxy_nodes_v31d3,step.proxy_nodes_v31d3);
    out.surface.proxy_strain_before_v31d3=std::max(out.surface.proxy_strain_before_v31d3,step.proxy_strain_before_v31d3);
    out.surface.proxy_strain_after_v31d3=std::max(out.surface.proxy_strain_after_v31d3,step.proxy_strain_after_v31d3);
    out.surface.proxy_budget_error_v31d3=std::max(out.surface.proxy_budget_error_v31d3,step.proxy_budget_error_v31d3);
    out.surface.melt_residual_peak_v30b=std::max(out.surface.melt_residual_peak_v30b,step.melt_residual_peak_v30b);
    out.surface.slope_scale=std::min(out.surface.slope_scale,step.slope_scale);
    out.surface.accepted_scale=std::min(out.surface.accepted_scale,step.accepted_scale);
    out.surface.transported_volume+=step.transported_volume;
    out.surface.signed_swept_volume+=step.signed_swept_volume;
    for(std::size_t i=0;i<work.size();++i) {
      work[i].position+=step.displacement[i];
      out.surface.displacement[i]+=step.displacement[i];
    }
    ++out.substeps;
  }
  if(!diagnose_refinement) return out;
  // Diagnose local resolution demand, not a native topology operation.
  // Count unique long or steep edges with at least one active endpoint.
  ClaySurfaceEdges &edges=dab_edges;
  if(!cache_topology){
  for(const auto &t:triangles)
    for(int j=0;j<3;++j) {
      auto a=t[j],b=t[(j+1)%3];
      if(a>b)std::swap(a,b);
      edges.push_back({a,b});
    }
  std::sort(edges.begin(),edges.end());
  edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  }
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
