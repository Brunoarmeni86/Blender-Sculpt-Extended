#pragma once
#include "artistic_material_v31d1110.hh"
namespace sculpt_physics::artistic_v31d1110 {
enum class Action { Build, Push, Smear, Gather, Press, Compress, Compact, Melt };
struct Intent {
 double native_deposit; // native Clay Strips target, in object length units
 double dose_length; // integrated stroke dose in object length units
 double pressure, compaction, grain_alignment;
};
struct Target {
 double normal_deposit, axial_sink, tangent_drag, radial_gather, transport, relaxation;
 double recoverable_sink;
};
// Scalar channels only: host supplies directions, conservative transport,
// permission and geometric constraints. No direct vertex writes.
inline Target target(Action a, Material m, const Intent& i) {
 if(!std::isfinite(i.native_deposit)||!std::isfinite(i.dose_length)||i.dose_length<0)
  throw std::invalid_argument("invalid intent");
 auto r=evaluate(m,i.pressure,i.compaction,i.grain_alignment);
 const double d=i.dose_length;
 Target t{};
 switch(a){
 case Action::Build:
  // Native target already contains native pressure/falloff; avoid applying
  // pressure twice. Material controls deposit and lateral redistribution.
  t.normal_deposit=i.native_deposit*profile(m).deposit/(1.+profile(m).hardening*std::clamp(i.compaction,0.,1.));
  t.transport=d*r.transport; break;
 case Action::Push: t.tangent_drag=d*r.transport; t.transport=d*r.transport; break;
 case Action::Smear: t.tangent_drag=d*r.drag; t.transport=.25*d*r.transport; break;
 case Action::Gather: t.radial_gather=d*r.drag; t.transport=d*r.transport; break;
 case Action::Press: t.axial_sink=d*r.sink; t.recoverable_sink=d*r.elastic_sink; t.transport=d*r.transport; break;
 case Action::Compress: t.axial_sink=d*r.sink; t.recoverable_sink=d*r.elastic_sink; t.transport=.5*d*r.transport; break;
 case Action::Compact: t.axial_sink=1.5*d*r.sink; t.recoverable_sink=1.5*d*r.elastic_sink; t.transport=.1*d*r.transport; break;
 case Action::Melt: t.relaxation=d*r.transport; break;
 default: throw std::invalid_argument("action");
 }
 return t;
}
}
