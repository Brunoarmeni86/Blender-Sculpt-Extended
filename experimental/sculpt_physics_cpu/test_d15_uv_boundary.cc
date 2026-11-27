/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d15_uv_boundary.hh"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace sculpt_physics::uv_boundary_d15;
static void check(bool b, const char *s)
{
  if (!b) { std::fprintf(stderr, "FAIL: %s\n", s); std::abort(); }
}
int main()
{
  Corners a{-1,-1}, b{-1,-1};
  check(ordered_corners(5,9,5,9,0,1,a) && a == Corners{0,1}, "forward corners");
  check(ordered_corners(5,9,9,5,2,3,b) && b == Corners{3,2}, "reversed winding");
  const UV values[]{{.2,.3},{.7,.8},{.7,.8},{.2,.3}};
  const Pair x{values[a[0]],values[a[1]]}, y{values[b[0]],values[b[1]]};
  check(classify(x,y)==Status::Continuous, "same endpoint UV despite reversed winding");
  check(!ordered_corners(5,9,5,8,2,3,b) && b==Corners{3,2}, "invalid mapping transactional");
  check(!ordered_corners(5,5,5,5,0,1,b), "degenerate edge");
  check(!ordered_corners(5,9,5,9,0,0,b), "degenerate corners");
  auto q=y;q[1][0]+=2e-7;
  check(classify(x,q)==Status::Continuous, "roundoff tolerance");
  q=y;q[0][1]+=.01;
  check(classify(x,q)==Status::Split, "one endpoint split");
  q=y;q[1][0]+=1.;
  check(classify(x,q)==Status::Split, "UDIM tile not wrapped");
  check(combine(classify(x,y),classify(x,q))==Status::Split, "secondary UV map split");
  q[0][0]=std::numeric_limits<double>::quiet_NaN();
  check(classify(x,q)==Status::Invalid && !valid(q), "NaN invalid");
  check(combine(Status::Split,classify(x,q))==Status::Invalid, "invalid layer dominates");
  q=y;q[1][1]=std::numeric_limits<double>::infinity();
  check(classify(x,q)==Status::Invalid, "infinite UV");
  check(classify(x,y,-1.)==Status::Invalid, "invalid tolerance");
  check(classify(x,y,0.)==Status::Continuous, "exact equality");
  std::puts("D15_UV_BOUNDARY_REFERENCE_OK");
}
