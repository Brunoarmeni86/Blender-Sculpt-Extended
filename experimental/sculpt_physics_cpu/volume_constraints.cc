/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "volume_constraints.hh"

#include <cmath>
#include <stdexcept>

namespace sculpt_physics {
namespace {
bool finite(Vec3 p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
double dot(Vec3 a, Vec3 b)
{
  return a.x*b.x + a.y*b.y + a.z*b.z;
}
Vec3 cross(Vec3 a, Vec3 b)
{
  return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
void validate_indices(std::size_t count, std::array<std::size_t, 4> ids)
{
  for (int i = 0; i < 4; ++i) {
    if (ids[i] >= count) throw std::out_of_range("Volume proxy tetrahedron OOB");
    for (int j = 0; j < i; ++j) {
      if (ids[i] == ids[j]) throw std::invalid_argument("Duplicate tetrahedron vertex");
    }
  }
}
}

double signed_tetra_volume(const std::array<Vec3, 4> &p)
{
  for (Vec3 v : p) {
    if (!finite(v)) throw std::invalid_argument("Non-finite tetrahedron coordinate");
  }
  const double volume = dot(p[1]-p[0], cross(p[2]-p[0], p[3]-p[0])) / 6.0;
  if (!std::isfinite(volume)) throw std::invalid_argument("Tetrahedron volume overflow");
  return volume;
}

TetraVolumeConstraint make_volume_constraint(
    const std::vector<Vec3> &rest,
    std::array<std::size_t, 4> ids,
    double compliance)
{
  validate_indices(rest.size(), ids);
  if (!std::isfinite(compliance) || compliance < 0.0) {
    throw std::invalid_argument("Invalid volume compliance");
  }
  const std::array<Vec3, 4> p{
    rest[ids[0]], rest[ids[1]], rest[ids[2]], rest[ids[3]]};
  const double v = signed_tetra_volume(p);
  if (v <= 1.0e-15) throw std::invalid_argument("Degenerate/inverted rest tetrahedron");
  return {ids, v, compliance, 0.0};
}

std::vector<Vec3> solve_tetra_volume_xpbd(
    const std::vector<Vec3> &positions,
    const std::vector<double> &inverse_masses,
    std::vector<TetraVolumeConstraint> &constraints,
    double dt,
    int iterations)
{
  if (positions.size() != inverse_masses.size() ||
      !std::isfinite(dt) || dt <= 0.0 ||
      iterations < 1 || iterations > 256) {
    throw std::invalid_argument("Invalid volume solver dimensions/time/iterations");
  }
  const double h2 = dt * dt;
  if (!std::isfinite(h2) || h2 <= 0.0) {
    throw std::invalid_argument("Volume timestep squared underflow/overflow");
  }
  for (std::size_t i = 0; i < positions.size(); ++i) {
    if (!finite(positions[i]) || !std::isfinite(inverse_masses[i]) ||
        inverse_masses[i] < 0.0) {
      throw std::invalid_argument("Invalid volume proxy particle");
    }
  }
  for (const auto &tet : constraints) {
    validate_indices(positions.size(), tet.indices);
    if (!std::isfinite(tet.rest_volume) || tet.rest_volume <= 1.0e-15 ||
        !std::isfinite(tet.compliance) || tet.compliance < 0.0 ||
        !std::isfinite(tet.lambda)) {
      throw std::invalid_argument("Invalid XPBD tetra constraint");
    }
  }
  // Stage BOTH geometry and lambdas; solver failure cannot damage a stroke.
  std::vector<Vec3> staged = positions;
  std::vector<TetraVolumeConstraint> staged_constraints = constraints;
  for (int iter = 0; iter < iterations; ++iter) {
    for (auto &tet : staged_constraints) {
      const auto &id = tet.indices;
      const Vec3 p0 = staged[id[0]], p1 = staged[id[1]];
      const Vec3 p2 = staged[id[2]], p3 = staged[id[3]];
      const Vec3 e1=p1-p0, e2=p2-p0, e3=p3-p0;
      const double volume = signed_tetra_volume({p0,p1,p2,p3});
      const Vec3 g1 = cross(e2,e3) / 6.0;
      const Vec3 g2 = cross(e3,e1) / 6.0;
      const Vec3 g3 = cross(e1,e2) / 6.0;
      const Vec3 g0 = (g1+g2+g3) * (-1.0);
      const std::array<Vec3, 4> grad{g0,g1,g2,g3};
      const double alpha = tet.compliance / h2;
      double denominator = alpha;
      for (int k = 0; k < 4; ++k) {
        denominator += inverse_masses[id[k]] * dot(grad[k], grad[k]);
      }
      if (!std::isfinite(denominator) || denominator < 0.0) {
        throw std::invalid_argument("Invalid volume XPBD denominator");
      }
      if (denominator == 0.0) continue; // All four particles pinned.
      const double d_lambda = (-(volume - tet.rest_volume) -
                               alpha * tet.lambda) / denominator;
      if (!std::isfinite(d_lambda)) {
        throw std::invalid_argument("Non-finite volume correction");
      }
      tet.lambda += d_lambda;
      if (!std::isfinite(tet.lambda)) throw std::invalid_argument("Volume lambda overflow");
      for (int k = 0; k < 4; ++k) {
        staged[id[k]] += grad[k] * (inverse_masses[id[k]] * d_lambda);
        if (!finite(staged[id[k]])) {
          throw std::invalid_argument("Non-finite volume position");
        }
      }
    }
  }
  constraints.swap(staged_constraints);
  return staged;
}

}  // namespace sculpt_physics
