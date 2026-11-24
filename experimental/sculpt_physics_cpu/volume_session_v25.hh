/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "volume_surface_v25.hh"
#include "material_point_reference_v21.hh"
#include "fluid_pbf_v23.hh"

namespace sculpt_physics {
enum class VolumeBackendV25 { LiquidPBF, ViscoplasticMPM };
struct VolumeSessionV25 {
  VolumeBackendV25 backend=VolumeBackendV25::LiquidPBF;
  double particle_spacing=.1;
  double density=1;
  double time=0;
  std::vector<FluidParticleV23> liquid;
  std::vector<MaterialPointV21> clay;
};
inline VolumeSessionV25 begin_volume_v25(const VolumeSurfaceV25 &mesh,
    VolumeBackendV25 backend,double spacing,double density=1){
  if(!std::isfinite(density)||density<=0)
    throw std::invalid_argument("Invalid volume material density");
  const auto points=seed_volume_v25(mesh,spacing);
  VolumeSessionV25 s;s.backend=backend;s.particle_spacing=spacing;s.density=density;
  const double v=spacing*spacing*spacing,m=density*v;
  if(!std::isfinite(m)||m<=0||!std::isfinite(v)||v<=0)
    throw std::invalid_argument("Invalid particle volume or mass");
  if(backend==VolumeBackendV25::LiquidPBF){
    for(auto p:points)s.liquid.push_back({p,{}});
  }else if(backend==VolumeBackendV25::ViscoplasticMPM){
    for(auto p:points){MaterialPointV21 q;q.position=p;q.mass=m;q.volume=q.rest_volume=v;s.clay.push_back(q);}
  }else throw std::invalid_argument("Unknown volume backend");
  return s;
}
struct VolumeFrameV25 {
  double dt=.001;
  int substeps=4;
  Vec3 gravity{0,0,-9.81};
  // Uniform softening knob, not a heat equation or phase transition.
  double softening=0;
  double liquid_viscosity=.1;
  bool tool_enabled=false;
  Vec3 tool_start{},tool_end{};
  double tool_radius=.2;
  bool floor_enabled=false;
  double floor_z=0.0;
};
inline VolumeSessionV25 stage_volume_frame_v25(const VolumeSessionV25 &before,
                                                const VolumeFrameV25 &f){
  const auto interval=[](double v){return std::isfinite(v)&&v>=0&&v<=1;};
  if(!std::isfinite(f.dt)||f.dt<=0||f.dt>.1||f.substeps<1||f.substeps>64||
     !interval(f.softening)||!interval(f.liquid_viscosity)||
     (f.floor_enabled&&!std::isfinite(f.floor_z))||
     !std::isfinite(volume_norm_v25(f.gravity))||
     !std::isfinite(before.particle_spacing)||before.particle_spacing<=1e-9||
     !std::isfinite(before.density)||before.density<=0||
     !std::isfinite(before.time)||before.time<0||
     (f.tool_enabled&&(!std::isfinite(volume_norm_v25(f.tool_start))||
                       !std::isfinite(volume_norm_v25(f.tool_end))||
                       !std::isfinite(f.tool_radius)||f.tool_radius<=0)))
    throw std::invalid_argument("Invalid volumetric frame");
  if((before.backend==VolumeBackendV25::LiquidPBF &&
      (before.liquid.empty()||!before.clay.empty()))||
     (before.backend==VolumeBackendV25::ViscoplasticMPM &&
      (before.clay.empty()||!before.liquid.empty())))
    throw std::invalid_argument("Invalid volume session state");
  VolumeSessionV25 out=before;
  const double dx=before.particle_spacing,dt=f.dt/double(f.substeps);
  const auto velocity=(f.tool_end-f.tool_start)/f.dt;
  for(int k=0;k<f.substeps;++k){
    const Vec3 center=f.tool_start+(f.tool_end-f.tool_start)*(double(k+1)/f.substeps);
    if(out.backend==VolumeBackendV25::LiquidPBF){
      FluidSettingsV23 s;s.support=2.0*dx;s.mass=before.density*dx*dx*dx;
      s.rest_density=before.density;s.dt=dt;s.gravity=f.gravity;
      s.viscosity=f.liquid_viscosity;s.tool_enabled=f.tool_enabled;
      s.tool_center=center;s.tool_radius=f.tool_radius;
      s.floor_enabled=f.floor_enabled;s.floor_z=f.floor_z;
      out.liquid=step_fluid_v23(out.liquid,s).particles;
    }else if(out.backend==VolumeBackendV25::ViscoplasticMPM){
      MaterialPointSettingsV21 s;s.spacing=2*dx;s.dt=dt;s.gravity=f.gravity;
      s.bulk=20;s.shear=8*(1-.95*f.softening);
      s.cohesion=.5*(1-f.softening);s.friction=.5*(1-.8*f.softening);
      s.viscosity=.1+.4*f.softening;
      s.floor_enabled=f.floor_enabled;s.floor_z=f.floor_z;
      MaterialPointToolV21 tool;tool.enabled=f.tool_enabled;
      tool.center=center;tool.velocity=velocity;tool.radius=f.tool_radius;
      out.clay=step_material_points_v21(out.clay,s,tool).particles;
    }else throw std::invalid_argument("Unknown volume backend");
  }
  out.time+=f.dt;
  if(!std::isfinite(out.time))throw std::overflow_error("Volume time overflow");
  // Atomic staging: exceptions leave input untouched. Persist particles,
  // velocities and stress across frames; no resampling each dab/frame.
  return out;
}
inline VolumeSurfaceV25 reconstruct_volume_v25(const VolumeSessionV25 &s,
                                               double spacing,double radius){
  std::vector<Vec3> points;
  if(s.backend==VolumeBackendV25::LiquidPBF){
    points.reserve(s.liquid.size());for(const auto&p:s.liquid)points.push_back(p.position);
  }else if(s.backend==VolumeBackendV25::ViscoplasticMPM){
    points.reserve(s.clay.size());for(const auto&p:s.clay)points.push_back(p.position);
  }else throw std::invalid_argument("Unknown volume backend");
  return extract_volume_v25(points,spacing,radius);
}
} // namespace sculpt_physics
