/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physical_brush_intent.hh"
#include <algorithm>
#include <cmath>

namespace sculpt_physics::dedicated_d12 {
inline double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline double norm2(Vec3 a){return dot(a,a);}
// Native samples are float32 promoted to double; squared lengths cannot
// overflow in that domain. Avoid the general three-argument hypot hot path.
inline double norm(Vec3 a){return std::sqrt(norm2(a));}
inline Vec3 unit(Vec3 a){const double q=norm2(a);return q>1e-24&&std::isfinite(q)?a/std::sqrt(q):Vec3{};}
inline Vec3 unit_normal(Vec3 a){const double q=norm2(a);return std::abs(q-1.)<1e-5?a:unit(a);}
inline Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool finite(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline bool valid_sample(const PhysicalBrushIntentSample& s){
 return s.writable&&s.envelope>0&&std::isfinite(s.envelope)&&s.envelope<=1.&&
        finite(s.position)&&finite(s.surface_normal)&&norm2(s.surface_normal)>1e-24;
}
}
