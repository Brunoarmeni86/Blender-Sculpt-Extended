/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "multires_stable_detail_transport.hh"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <random>
using V = std::array<float, 3>;
using M = std::array<V, 3>;
const M identity = {{{1,0,0},{0,1,0},{0,0,1}}};
double length(const V &v) { return std::hypot(double(v[0]), double(v[1]), double(v[2])); }
bool near(const V &a, const V &b)
{
  return length({a[0]-b[0], a[1]-b[1], a[2]-b[2]}) <= 2e-6 * std::max(1.0, length(b));
}
int main()
{
  using blender::multires_stable_detail::transport;
  const V detail = {0.04f, -0.02f, 0.3f};
  assert(near(transport(identity, identity, detail), detail));
  M rotate = {{{0,1,0},{-1,0,0},{0,0,1}}};
  assert(near(transport(identity, rotate, detail), V{0.02f,0.04f,0.3f}));
  assert(near(transport(rotate, identity, transport(identity, rotate, detail)), detail));
  M collapse = {{{1,0,0},{1,1e-10f,0},{0,0,1}}};
  assert(near(transport(collapse, identity, detail), detail));
  assert(near(transport(identity, collapse, detail), detail));
  M zero = {};
  assert(near(transport(zero, identity, detail), detail));
  M invalid = identity;
  invalid[0][0] = std::numeric_limits<float>::infinity();
  assert(near(transport(invalid, identity, detail), detail));
  M skew = {{{1,0,0},{1,1e-4f,0},{0,0,1}}};
  auto stable = transport(skew, identity, detail);
  assert(std::abs(length(stable)-length(detail)) < 1e-6);
  // Analytic native inverse for the same skew matrix.
  V native = {detail[0]-detail[1]/1e-4f, detail[1]/1e-4f, detail[2]};
  std::cout << "skew case: native gain=" << length(native)/length(detail)
            << ", stable gain=" << length(stable)/length(detail) << '\n';
  std::mt19937 rng(427);
  std::uniform_real_distribution<float> random(-1,1);
  for (int i=0; i<100000; i++) {
    M a, b;
    for (auto *matrix : {&a,&b}) {
      for (auto &column : *matrix) {
        for (float &value : column) value=random(rng);
      }
    }
    V d={random(rng),random(rng),random(rng)};
    V result=transport(a,b,d);
    assert(std::isfinite(length(result)));
    assert(std::abs(length(result)-length(d)) < 1e-6);
    assert(near(transport(b,a,result),d));
  }
  // Intentional large spikes must keep their amplitude.
  V spike={0,0,1000};
  assert(near(transport(skew,rotate,spike),spike));
  std::cout << "PASS: identity, rotation, inverse, collapse, nonfinite frames, skew, 100000 random pairs, intentional spike\n";
}
