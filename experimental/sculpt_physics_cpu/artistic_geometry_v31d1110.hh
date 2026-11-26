#pragma once
#include "hybrid_foundation_v31d11.hh"
#include "artistic_tool_v31d1110.hh"
namespace sculpt_physics::artistic_v31d1110 {
namespace h=hybrid_v31d11;
// Equal/opposite area-weighted transport proposal; not exact mesh volume.
inline std::vector<double> pair_transport(const h::Patch& s,
 const std::vector<double>& drive,double radius,double dose,
 h::GeodesicCache* cache=nullptr) {
 if(!std::isfinite(dose)||dose<0||!std::isfinite(radius)||radius<=0||
    drive.size()!=s.p.size())throw std::invalid_argument("transport inputs");
 bool have=false,nonuniform=false;
 double first=0.;
 for(std::size_t i=0;i<drive.size();++i){
  if(!std::isfinite(drive[i]))throw std::invalid_argument("transport drive");
  if(s.weight[i]<=0||s.area[i]<=0)continue;
  if(!have){first=drive[i];have=true;}
  else if(drive[i]!=first)nonuniform=true;
 }
 if(dose==0||!nonuniform)return std::vector<double>(s.p.size());
 auto flux=cache?h::cached_geodesic_flux(*cache,s,drive,radius).flux:
                  h::transport_flux(h::transport_stencil(s,radius),drive);
 for(auto& x:flux)x*=dose/radius;
 return flux;
}
// Fast conservative acceptance: one common scale retains the signed
// normal-area proxy of paired transport. No iterative recovery. The native
// writer still validates its own final region, clipping and Undo coverage.
inline std::vector<Vec3> accept_fast(const h::Patch& patch,std::vector<Vec3> desired){
 if(desired.size()!=patch.p.size())throw std::invalid_argument("target shape");
 for(std::size_t i=0;i<desired.size();++i){
  if(!std::isfinite(h::norm(desired[i])))throw std::invalid_argument("nonfinite target");
  if(patch.weight[i]<=0)desired[i]={};
 }
 const double scale=gradient_motion_scale_v31c(patch.p,patch.tris,desired);
 for(auto& d:desired)d=d*scale;
 limit_local_contact(patch.p,patch.tris,desired,true);
 return desired;
}
// Distinct pressure tools: Press approaches a tool plane, Compress
// moves along the tool axis, Compact sinks locally with little lateral flow.
// All remain surface proxies. No elastic recovery until persistent state.
inline std::vector<Vec3> pressure_target(const h::Patch& patch,Action action,
 Material material,Vec3 center,Vec3 axis,double radius,double dose,double pressure,
 double plane_height,const std::vector<double>& compaction,
 h::GeodesicCache* cache=nullptr,bool fast=false){
 if(action!=Action::Press&&action!=Action::Compress&&action!=Action::Compact)
  throw std::invalid_argument("pressure action");
 if(!std::isfinite(radius)||radius<=0||!std::isfinite(dose)||dose<0||
    !std::isfinite(plane_height)||!std::isfinite(h::norm(center))||
    !std::isfinite(h::norm(axis))||h::norm(axis)<1e-12||
    compaction.size()!=patch.p.size())throw std::invalid_argument("pressure input");
 axis=h::unit(axis);
 std::vector<Vec3> out(patch.p.size());
 std::vector<double> drive(out.size());
 for(std::size_t i=0;i<out.size();++i){
  if(patch.weight[i]<=0)continue;
  const auto r=evaluate(material,pressure,compaction[i],1.);
  const double e=dose*patch.weight[i];
  const auto normal=patch.normal[i];
  if(action==Action::Press){
   const double gap=std::max(0.,h::dot(patch.p[i]-center,axis)-plane_height);
   const double sink=std::min(gap,gap*(-std::expm1(-4.*e/radius*r.sink)));
   out[i]=axis*(-sink);
   drive[i]=patch.weight[i]*r.transport*std::clamp(gap/radius,0.,1.);
  }else if(action==Action::Compress){
   // Continuous tool-axis action on narrow walls, including vertical faces.
   out[i]=axis*(-e*r.sink);
   drive[i]=.5*patch.weight[i]*r.transport;
  }else{
   out[i]=normal*(-1.5*e*r.plastic_sink);
   drive[i]=.05*patch.weight[i]*r.transport;
  }
 }
 if(action!=Action::Compact){
  auto flux=pair_transport(patch,drive,radius,dose,cache);
  for(std::size_t i=0;i<out.size();++i)out[i]+=patch.normal[i]*flux[i];
 }
 return fast?accept_fast(patch,std::move(out)):h::optimize(patch,std::move(out),true);
}
// Opt-in reference adapter. Native deposition already includes host factors;
// other channels use the patch weight once. No direct editor writeback.
inline std::vector<Vec3> geometry_target(const h::Patch& s, Action action,
 Material material, const std::vector<double>& native_deposit,
 const std::vector<double>& compaction, Vec3 axis, Vec3 center,
 double dose_length,double pressure,double transport_radius=0,
 const std::vector<double>* exposure=nullptr,h::GeodesicCache* cache=nullptr) {
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
  auto flux=pair_transport(s,transport_drive,transport_radius,dose_length,cache);
  for(std::size_t i=0;i<n;++i)desired[i]+=s.normal[i]*flux[i];
 }
 return h::optimize(s,std::move(desired),transport_radius>0);
}
}
