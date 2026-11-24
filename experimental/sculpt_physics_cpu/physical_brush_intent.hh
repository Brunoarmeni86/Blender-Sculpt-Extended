/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "native_grab_executor_adapter.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* V17-A: pure brush input -> displacement intent. No native translation
 * dependency, no World allocation, no editor writes and no material history.
 * This is a shared front end, NOT a constitutive law or a completed solver.
 * The caller must gather writable candidates BEFORE native geometry changes,
 * evaluate Blender's envelope once, and honor native Undo coverage. */
enum class PhysicalBrushAction { Deposit, Compress, Flow };

struct PhysicalBrushIntentSample {
  std::size_t global_index = 0;
  Vec3 position{};
  // Final geometric influence [0,1]: masks, falloff, alpha, etc.
  // Do not include the dab pressure/intensity again here.
  double envelope = 0.0;
  // Positive local surface area weight; caller supplies actual area or
  // a documented approximation. Only relative weights matter.
  double area = 1.0;
  bool writable = false;
};

struct PhysicalBrushIntentDab {
  PhysicalBrushAction action = PhysicalBrushAction::Deposit;
  Vec3 center{};
  Vec3 normal{0.0, 0.0, 1.0};
  Vec3 stroke_direction{};
  double radius = 1.0;
  double intensity = 1.0; // normalized pressure * material action strength
  double dose = 0.0;      // normalized arc length or explicit dwell increment
  double spread = 0.55;
  // Native callers already filtered the exact brush footprint (e.g. cube).
  bool limit_to_projected_disk = true;
};

/* The increment is bounded by a fraction of the brush radius, not by edge
 * spacing or the magnitude of Blender's original deformation. Compression
 * balances area-weighted signed TARGET displacement along one brush axis,
 * not actual enclosed volume after the solver. Flow requires a direction.
 * Arc-length resampling and dwell policy are intentionally editor-owned.
 * A zero dose/pressure, fully masked sample or nonwritable sample is a no-op.
 * Results are unique and sorted by global ID, for the compact adapter. */
inline std::vector<NativeBrushTranslation> plan_physical_brush_intent(
    const std::vector<PhysicalBrushIntentSample> &samples,
    const PhysicalBrushIntentDab &dab)
{
  const auto finite=[](Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  const auto dot=[](Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  const auto norm=[](Vec3 v) { return std::hypot(v.x,v.y,v.z); };
  const auto unit_interval=[](double x) { return std::isfinite(x) && x>=0.0 && x<=1.0; };
  if (!finite(dab.center) || !finite(dab.normal) || !finite(dab.stroke_direction) ||
      !std::isfinite(dab.radius) || dab.radius<=0.0 ||
      !unit_interval(dab.intensity) || !unit_interval(dab.dose) ||
      !unit_interval(dab.spread)) {
    throw std::invalid_argument("Invalid independent brush intent");
  }
  if (dab.action!=PhysicalBrushAction::Deposit &&
      dab.action!=PhysicalBrushAction::Compress &&
      dab.action!=PhysicalBrushAction::Flow) {
    throw std::invalid_argument("Unknown physical brush action");
  }
  const double normal_len=norm(dab.normal);
  if (!std::isfinite(normal_len) || normal_len<=1e-12) {
    throw std::invalid_argument("Invalid physical brush normal");
  }
  const Vec3 axis=dab.normal/normal_len;
  std::vector<std::size_t> order;
  order.reserve(samples.size());
  double area_max=0.0;
  for (std::size_t i=0;i<samples.size();++i) {
    const auto &s=samples[i];
    if (!finite(s.position) || !unit_interval(s.envelope) ||
        !std::isfinite(s.area) || s.area<=0.0) {
      throw std::invalid_argument("Invalid physical brush sample");
    }
    order.push_back(i);
    if (s.writable && s.envelope>0.0) area_max=std::max(area_max,s.area);
  }
  std::sort(order.begin(),order.end(),[&](auto a,auto b) {
    return samples[a].global_index<samples[b].global_index;
  });
  for (std::size_t i=1;i<order.size();++i) {
    if (samples[order[i-1]].global_index==samples[order[i]].global_index) {
      throw std::invalid_argument("Duplicate independent brush sample");
    }
  }
  const double step=(dab.radius*0.1)*dab.intensity*dab.dose;
  if (step==0.0 || area_max==0.0) return {};
  const Vec3 tangent=dab.stroke_direction-axis*dot(dab.stroke_direction,axis);
  const double tangent_len=norm(tangent);
  if (!std::isfinite(tangent_len)) throw std::overflow_error("Invalid brush tangent");
  if (dab.action==PhysicalBrushAction::Flow && tangent_len<=1e-12) return {};
  const Vec3 direction=tangent_len>1e-12 ? tangent/tangent_len : Vec3{};
  struct Active { std::size_t index; double rim; };
  std::vector<Active> active;
  active.reserve(samples.size());
  double weight=0.0,rim_weight=0.0;
  for (const auto i : order) {
    const auto &s=samples[i];
    if (!s.writable || s.envelope==0.0) continue;
    const Vec3 offset=(s.position-dab.center)/dab.radius;
    const double radius=norm(offset-axis*dot(offset,axis));
    if (!std::isfinite(radius)) throw std::overflow_error("Invalid brush footprint");
    if (dab.limit_to_projected_disk && radius>1.0) continue;
    double rim=std::clamp((radius-0.25)/0.55,0.0,1.0);
    rim=rim*rim*(3.0-2.0*rim);
    const double w=(s.area/area_max)*s.envelope;
    weight+=w;
    rim_weight+=w*rim;
    active.push_back({i,rim});
  }
  if (weight<=0.0) return {};
  const double mean=rim_weight/weight;
  std::vector<NativeBrushTranslation> result;
  result.reserve(active.size());
  for (const auto &a : active) {
    const auto &s=samples[a.index];
    const double h=step*s.envelope;
    Vec3 move{};
    switch (dab.action) {
      case PhysicalBrushAction::Deposit:
        move=axis*(h*(1.0+dab.spread*(a.rim-mean)));
        break;
      case PhysicalBrushAction::Compress:
        move=axis*(h*dab.spread*(a.rim-mean));
        break;
      case PhysicalBrushAction::Flow:
        move=direction*h;
        break;
    }
    if (!finite(move)) throw std::overflow_error("Invalid independent brush motion");
    if (norm(move)>0.0) result.push_back({s.global_index,move});
  }
  return result;
}

}  // namespace sculpt_physics
