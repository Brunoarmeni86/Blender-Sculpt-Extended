/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cmath>

namespace sculpt_physics::uv_boundary_d15 {
using Corners = std::array<int, 2>;
using UV = std::array<double, 2>;
using Pair = std::array<UV, 2>;
enum class Status { Continuous, Split, Invalid };

// Match corner samples by the mesh edge's endpoint identity, not the
// winding of either incident face. Refuse malformed corner-edge mappings.
inline bool ordered_corners(int a, int b, int v0, int v1,
                            int c0, int c1, Corners &out)
{
  if (a < 0 || b < 0 || a == b || c0 < 0 || c1 < 0 || c0 == c1) return false;
  if (v0 == a && v1 == b) { out = {c0, c1}; return true; }
  if (v0 == b && v1 == a) { out = {c1, c0}; return true; }
  return false;
}

inline bool valid(const Pair &uv)
{
  for (const auto &p : uv) for (double x : p) if (!std::isfinite(x)) return false;
  return true;
}

// Absolute tolerance in UV units. Never wrap across integer tiles (UDIM).
// Both endpoints of the edge must agree in the same UV map.
inline Status classify(const Pair &a, const Pair &b, double epsilon = 1e-6)
{
  if (!valid(a) || !valid(b) || !std::isfinite(epsilon) || epsilon < 0.)
    return Status::Invalid;
  for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j)
    if (std::abs(a[i][j] - b[i][j]) > epsilon) return Status::Split;
  return Status::Continuous;
}

// Conservative union of layers; invalid data dominates split data.
inline Status combine(Status a, Status b)
{
  if (a == Status::Invalid || b == Status::Invalid) return Status::Invalid;
  if (a == Status::Split || b == Status::Split) return Status::Split;
  return Status::Continuous;
}
} // namespace sculpt_physics::uv_boundary_d15
