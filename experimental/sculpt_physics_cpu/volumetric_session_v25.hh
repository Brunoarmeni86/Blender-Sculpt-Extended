/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "volumetric_surface_v25.hh"
#include "material_point_reference_v21.hh"
#include "fluid_pbf_v23.hh"

namespace sculpt_physics {

/* Explicit, caller-owned snapshot. Backend choice is stable within a
 * transaction; switching material models needs a separate conversion.
 * Only the selected backend is evaluated. No Blender ownership or Undo
 * side effects. Particle ordering must remain stable for provenance. */
enum class VolumeBackendV25 { MPM, PBF };
struct VolumeSessionV25 {
  VolumeBackendV25 backend=VolumeBackendV25::MPM;
  std::vector<MaterialPointV21> solid;
  std::vector<FluidParticleV23> liquid;
  double liquid_radius=.1;
};
struct VolumeTrialV25 {
  VolumeSessionV25 next;
  VolumeSurfaceV25 surface;
  double mass=0;
};
inline VolumeTrialV25 stage_volume_trial_v25(
    const VolumeSessionV25 &before,
    const MaterialPointSettingsV21 &mpm,
    const MaterialPointToolV21 &tool,
    const FluidSettingsV23 &fluid,
    int steps,double extraction_spacing,std::size_t budget=1000000)
{
  if(steps<1||steps>32)throw std::invalid_argument("Invalid volume substeps");
  VolumeTrialV25 out;out.next=before;
  std::vector<VolumeSampleV25> samples;
  if(before.backend==VolumeBackendV25::MPM){
    if(!before.liquid.empty())throw std::invalid_argument("Mixed backend snapshot");
    for(int i=0;i<steps;++i){
      auto next=step_material_points_v21(out.next.solid,mpm,tool);
      out.mass=next.mass;out.next.solid=std::move(next.particles);
    }
    // Overlapping spherical supports are only an approximate visual
    // reconstruction of the particles; mesh volume need not equal mass/rho.
    for(const auto &p:out.next.solid)
      samples.push_back({p.position,.85*std::cbrt(p.volume)});
  }
  else if(before.backend==VolumeBackendV25::PBF){
    if(!before.solid.empty()||!std::isfinite(before.liquid_radius)||before.liquid_radius<=0)
      throw std::invalid_argument("Invalid liquid snapshot");
    for(int i=0;i<steps;++i){
      auto next=step_fluid_v23(out.next.liquid,fluid);
      out.mass=next.mass;out.next.liquid=std::move(next.particles);
    }
    for(const auto &p:out.next.liquid)samples.push_back({p.position,before.liquid_radius});
  }
  else throw std::invalid_argument("Unknown volume backend");
  out.surface=extract_volume_surface_v25(samples,extraction_spacing,budget);
  return out;
}
} // namespace sculpt_physics
