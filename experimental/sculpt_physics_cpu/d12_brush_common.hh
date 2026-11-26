/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physical_brush_intent.hh"
#include <algorithm>
#include <cmath>

namespace sculpt_physics::dedicated_d12 {
inline double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline double norm(Vec3 a){return std::hypot(a.x,a.y,a.z);}
inline Vec3 unit(Vec3 a){const double l=norm(a);return l>1e-12?a/l:Vec3{};}
inline Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool finite(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline bool valid_sample(const PhysicalBrushIntentSample& s){
 return s.writable&&s.envelope>0&&std::isfinite(s.envelope)&&s.envelope<=1.&&
        finite(s.position)&&finite(s.surface_normal)&&norm(s.surface_normal)>1e-12;
}
}
