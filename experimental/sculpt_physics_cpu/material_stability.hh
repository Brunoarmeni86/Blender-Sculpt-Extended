/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_workset.hh"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* V18-A surface-motion safety pass. Not a volumetric material law.
 * Bound the RELATIVE displacement of an edge's endpoints by a fraction
 * of its CURRENT length, so along the linear dab path the edge cannot
 * contract or expand by more than that fraction. This is a per-dab
 * bound, not a cumulative rest-strain, triangle-area or collision bound.
 * All exterior/pinned vertices remain exactly fixed. */
struct MaterialStabilitySettings {
  bool enabled = false;
  double relative_limit = 0.20;
  double relaxation = 0.15;
  int iterations = 3;
};

struct MaterialStabilityStats {
  double final_scale = 1.0;
  double peak_relative_motion = 0.0;
};

inline void validate_material_stability(const MaterialStabilitySettings &s)
{
  if (!std::isfinite(s.relative_limit) || s.relative_limit<=0.0 ||
      s.relative_limit>=1.0 || !std::isfinite(s.relaxation) ||
      s.relaxation<0.0 || s.relaxation>1.0 ||
      s.iterations<0 || s.iterations>16) {
    throw std::invalid_argument("Invalid material stability settings");
  }
}

inline MaterialStabilityStats stabilize_surface_motion(
    const GpuLocalIsland &island,
    std::vector<Vec3> &motion,
    const MaterialStabilitySettings &settings,
    bool relax)
{
  validate_material_stability(settings);
  const std::size_t n=island.positions.size();
  if (motion.size()!=n || island.writable.size()!=n ||
      island.inverse_masses.size()!=n) {
    throw std::invalid_argument("Invalid stability island");
  }
  const auto norm=[](Vec3 v) {return std::hypot(v.x,v.y,v.z);};
  const auto movable=[&](std::size_t i) {
    return island.writable[i] && island.inverse_masses[i]>0.0;
  };
  for (std::size_t i=0;i<n;++i) {
    if (!std::isfinite(norm(motion[i])) || !std::isfinite(norm(island.positions[i])) ||
        !std::isfinite(island.inverse_masses[i]) || island.inverse_masses[i]<0.0) {
      throw std::invalid_argument("Invalid stability motion");
    }
    if (!movable(i)) motion[i]={};
  }
  std::vector<double> lengths;
  lengths.reserve(island.bonds.size());
  for (const auto &b : island.bonds) {
    if (b.a>=n || b.b>=n || b.a==b.b) throw std::invalid_argument("Invalid stability edge");
    const double len=norm(island.positions[b.a]-island.positions[b.b]);
    if (!std::isfinite(len)) throw std::overflow_error("Invalid stability edge length");
    lengths.push_back(len);
  }
  // Jacobi relaxation of incremental motion, not absolute positions:
  // a rigid translation stays rigid, existing detail is not smoothed
  // away by a global Laplacian on the surface itself.
  if (relax && settings.relaxation>0.0) {
    std::vector<Vec3> sum(n);
    std::vector<unsigned int> degree(n,0);
    for (int pass=0;pass<settings.iterations;++pass) {
      std::fill(sum.begin(),sum.end(),Vec3{});
      std::fill(degree.begin(),degree.end(),0);
      for (const auto &b : island.bonds) {
        if (movable(b.a)) {sum[b.a]+=motion[b.b]; ++degree[b.a];}
        if (movable(b.b)) {sum[b.b]+=motion[b.a]; ++degree[b.b];}
      }
      for (std::size_t i=0;i<n;++i) {
        if (movable(i) && degree[i]>0) {
          const Vec3 mean=sum[i]/double(degree[i]);
          motion[i]=motion[i]*(1.0-settings.relaxation)+mean*settings.relaxation;
        }
      }
    }
  }
  // Local projection of excessive RELATIVE motion. Corrections only
  // affect mobile endpoints, and preserve pair mean when both are free.
  // A final conservative global scale enforces every remaining bound
  // even if the finite number of local sweeps has not converged.
  for (int pass=0;pass<settings.iterations;++pass) {
    for (std::size_t k=0;k<island.bonds.size();++k) {
      const auto &b=island.bonds[k];
      const bool a=movable(b.a),c=movable(b.b);
      if (!a && !c) continue;
      const Vec3 diff=motion[b.b]-motion[b.a];
      const double relative=norm(diff);
      const double limit=lengths[k]*settings.relative_limit;
      if (!std::isfinite(relative)) throw std::overflow_error("Stability motion overflow");
      if (relative<=limit || relative==0.0) continue;
      const Vec3 correction=diff*(1.0-limit/relative);
      const double split=a && c ? 0.5 : 1.0;
      if (a) motion[b.a]+=correction*split;
      if (c) motion[b.b]=motion[b.b]-correction*split;
    }
  }
  MaterialStabilityStats out;
  for (std::size_t k=0;k<island.bonds.size();++k) {
    const auto &b=island.bonds[k];
    const double relative=norm(motion[b.b]-motion[b.a]);
    if (!std::isfinite(relative)) throw std::overflow_error("Stability projection overflow");
    if (relative>0.0) {
      out.final_scale=std::min(out.final_scale,lengths[k]*settings.relative_limit/relative);
    }
  }
  for (auto &v : motion) {
    v=v*out.final_scale;
    if (!std::isfinite(norm(v))) throw std::overflow_error("Stability result overflow");
  }
  for (std::size_t k=0;k<island.bonds.size();++k) {
    const auto &b=island.bonds[k];
    if (lengths[k]>0.0) {
      out.peak_relative_motion=std::max(out.peak_relative_motion,
          norm(motion[b.b]-motion[b.a])/lengths[k]);
    }
  }
  return out;
}

} // namespace sculpt_physics
