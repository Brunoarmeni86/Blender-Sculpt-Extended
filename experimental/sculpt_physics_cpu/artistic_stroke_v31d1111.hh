#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstddef>
namespace sculpt_physics::artistic_v31d1110 {
struct StrokeSegment {
 double distance, seconds, pressure_start, pressure_end, radius;
};
struct StrokeDose { double moving, stationary; std::size_t subdivisions; };
// Integrated linear pressure; additive for subdivisions of the same segment.
// Distance and stationary exposure are separate channels. Caller chooses
// action-specific time accumulation; no automatic double application.
inline StrokeDose integrate(StrokeSegment s) {
 if(!std::isfinite(s.distance)||!std::isfinite(s.seconds)||
    !std::isfinite(s.pressure_start)||!std::isfinite(s.pressure_end)||
    !std::isfinite(s.radius)||s.distance<0||s.seconds<0||s.radius<=0)
  throw std::invalid_argument("invalid stroke segment");
 double p=.5*(std::clamp(s.pressure_start,0.,1.)+std::clamp(s.pressure_end,0.,1.));
 double steps=std::ceil(s.distance/s.radius*8.);
 if(!std::isfinite(steps)||steps>4096)throw std::invalid_argument("stroke work budget");
 return {s.distance*p,s.seconds*p,static_cast<std::size_t>(std::max(1.,steps))};
}
}
