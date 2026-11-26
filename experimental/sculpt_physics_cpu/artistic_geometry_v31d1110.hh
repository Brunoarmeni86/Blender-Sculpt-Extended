#pragma once
#include "hybrid_foundation_v31d11.hh"
#include "artistic_tool_v31d1110.hh"
namespace sculpt_physics::artistic_v31d1110 {
namespace h=hybrid_v31d11;
// Equal/opposite area-weighted transport proposal; not exact mesh volume.
inline std::vector<double> pair_transport(const h::Patch& s,
 const std::vector<double>& drive,double radius,double dose) {
 if(!std::isfinite(dose)||dose<0)throw std::invalid_argument("transport dose");
 auto flux=h::transport_flux(h::transport_stencil(s,radius),drive);
 for(auto& x:flux)x*=dose/radius;
 return flux;
}
// Opt-in reference adapter. Native deposition already includes host factors;
// other channels use the patch weight once. No direct editor writeback.
inline std::vector<Vec3> geometry_target(const h::Patch& s, Action action,
 Material material, const std::vector<double>& native_deposit,
 const std::vector<double>& compaction, Vec3 axis, Vec3 center,
 double dose_length,double pressure,double transport_radius=0,
 const std::vector<double>* exposure=nullptr) {
 const auto n=s.p.size();
 if(native_deposit.size()!=n||compaction.size()!=n||(exposure&&exposure->size()!=n)||
 !std::isfinite(h::norm(axis))||!std::isfinite(h::norm(center)))
  throw std::invalid_argument("artistic patch inputs");
 axis=h::unit(axis);
 std::vector<Vec3> desired(n);
 std::vector<double> transport_drive(n);
 for(std::size_t i=0;i<n;++i){
  if(s.weight[i]<=0)continue;
  const auto normal=s.normal[i];
  const auto tangent=axis-normal*h::dot(axis,normal);
  const double local_dose=exposure?(*exposure)[i]:dose_length*s.weight[i];
  auto t=target(action,material,{native_deposit[i],local_dose,
                                 pressure,compaction[i],std::abs(h::dot(axis,h::unit(tangent)))});
  if(dose_length>0)transport_drive[i]=t.transport/dose_length;
  auto radial=center-s.p[i];radial=radial-normal*h::dot(radial,normal);
  auto inward=action==Action::Compress&&h::dot(axis,normal)>0?axis:normal;
  desired[i]=normal*t.normal_deposit-inward*t.axial_sink+
             tangent*t.tangent_drag+h::unit(radial)*t.radial_gather;
  if(action==Action::Melt){
   Vec3 avg{};double count=0;
   for(auto j:s.adj[i])if(s.weight[j]>0){avg+=s.p[j]-s.p[i];++count;}
   if(count>0)desired[i]+=normal*std::clamp(h::dot(avg/count,normal),-t.relaxation,t.relaxation);
  }
 }
 // Explicit radius enables opt-in transport. Zero preserves old behavior.
 if(!std::isfinite(transport_radius)||transport_radius<0)
  throw std::invalid_argument("transport radius");
 if(transport_radius>0&&dose_length>0){
  auto flux=pair_transport(s,transport_drive,transport_radius,dose_length);
  for(std::size_t i=0;i<n;++i)desired[i]+=s.normal[i]*flux[i];
 }
 return h::optimize(s,std::move(desired),transport_radius>0);
}
}
