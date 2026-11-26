#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sculpt_physics::artistic_v31d1110 {
// Dimensionless artistic coefficients, not measured constitutive constants.
// Standalone opt-in reference; host integration and calibration pending.
enum class Material { SoftClay, FirmClay, Mud, Wax, Flesh, Foam, Stone, Wood, Putty };
struct Profile {
 double yield, deposit, flow, adhesion, compression, recovery, hardening, anisotropy;
};
inline Profile profile(Material m) {
 switch(m) {
 case Material::SoftClay: return {.05,1.,.75,.65,.40,.05,.35,0.};
 case Material::FirmClay: return {.14,1.,.35,.55,.25,.04,.65,0.};
 case Material::Mud: return {.02,.85,1.,.35,.18,.01,.05,0.};
 case Material::Wax: return {.20,.8,.18,.75,.10,.08,.30,0.};
 case Material::Flesh: return {.08,.55,.25,.30,.45,.75,.08,0.};
 case Material::Foam: return {.04,.45,.08,.15,.85,.85,.12,0.};
 case Material::Stone: return {.65,.12,.02,.08,.02,.02,.90,0.};
 case Material::Wood: return {.45,.22,.12,.25,.06,.10,.65,.80};
 case Material::Putty: return {.09,.9,.55,.95,.30,.12,.20,0.};
 }
 throw std::invalid_argument("material");
}
struct Response {
 double deposit, transport, drag, sink, elastic_sink, plastic_sink;
};
// Input drive is separate from native permission and falloff. Caller must
// apply those once, and validate the final displacement with the host writer.
// grain_alignment is absolute tangent/grain dot product, not view alignment.
inline Response evaluate(Material m, double drive, double compaction,
                         double grain_alignment) {
 if(!std::isfinite(drive)||!std::isfinite(compaction)||!std::isfinite(grain_alignment))
  throw std::invalid_argument("nonfinite artistic response");
 auto p=profile(m);
 drive=std::clamp(drive,0.,1.);
 compaction=std::clamp(compaction,0.,1.);
 grain_alignment=std::clamp(grain_alignment,0.,1.);
 const double resistance=1.+p.hardening*compaction;
 const double plastic=std::max(0.,drive-p.yield)/(1.-p.yield)/resistance;
 const double directional=1.-p.anisotropy*(1.-grain_alignment);
 const double sink=p.compression*(plastic*(1.-p.recovery)+drive*p.recovery);
 return {drive*p.deposit/resistance,plastic*p.flow*directional,
         plastic*p.adhesion*directional,sink,
         drive*p.compression*p.recovery,
         plastic*p.compression*(1.-p.recovery)};
}
// Persistent, dimensionless compaction proxy. Update only from actual
// accepted inward displacement, never requested displacement. The elastic
// fraction must not accumulate as permanent compaction.
inline double compaction_after_write(Material m,double old,double inward,double radius,
 double rate_multiplier=1.){
 if(!std::isfinite(old)||old<0||old>1||!std::isfinite(inward)||
    !std::isfinite(radius)||radius<=0||!std::isfinite(rate_multiplier)||rate_multiplier<0)
  throw std::invalid_argument("artistic compaction input");
 const auto p=profile(m);
 const double rate=4.*p.compression*(1.-p.recovery)*rate_multiplier;
 const double dose=std::max(0.,inward)/radius;
 return old+(1.-old)*(-std::expm1(-rate*dose));
}
} // namespace sculpt_physics::artistic_v31d1110
