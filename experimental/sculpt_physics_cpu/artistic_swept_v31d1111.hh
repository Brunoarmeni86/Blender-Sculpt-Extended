#pragma once
#include "artistic_geometry_v31d1110.hh"
#include "artistic_stroke_v31d1111.hh"
namespace sculpt_physics::artistic_v31d1110 {
struct SweptExposure {
 std::vector<double> moving, time;
};
// Read-only reference integration on a fixed patch. Host must supply
// confirmed surface seeds and visibility; no PBVH or raycast implied.
inline SweptExposure swept_exposure(const h::Patch& patch,
 const std::vector<std::size_t>& seeds,Vec3 start,Vec3 end,
 double radius,double reach,double seconds,double p0,double p1,
 const std::vector<double>& visibility,const std::vector<double>& permission){
 const auto delta=end-start;
 const double length=h::norm(delta);
 const auto dose=integrate({length,seconds,p0,p1,radius});
 const auto n=patch.p.size();
 if(n>8192||dose.subdivisions>256)throw std::invalid_argument("swept patch budget");
 SweptExposure out{std::vector<double>(n),std::vector<double>(n)};
 p0=std::clamp(p0,0.,1.);p1=std::clamp(p1,0.,1.);
 for(std::size_t k=0;k<dose.subdivisions;++k){
  const double a=double(k)/dose.subdivisions,b=double(k+1)/dose.subdivisions;
  const double p=p0+(p1-p0)*(a+b)*.5;
  auto c=h::swept_contact(patch,seeds,start+delta*a,start+delta*b,
                          radius,reach,visibility,permission);
  for(std::size_t i=0;i<n;++i){
   out.moving[i]+=c.weight[i]*p*length/dose.subdivisions;
   out.time[i]+=c.weight[i]*p*seconds/dose.subdivisions;
  }
 }
 return out;
}
 // Reference bridge: geometric exposure is unpressurized so the material
 // law applies pressure once. Mean pressure is an approximation for a segment.
 // Native deposition is supplied already integrated by the host.
 inline std::vector<Vec3> swept_geometry_target(const h::Patch& patch,
 Action action,Material material,const std::vector<double>& native_deposit,
 const std::vector<double>& compaction,const std::vector<std::size_t>& seeds,
 Vec3 start,Vec3 end,Vec3 tool_axis,double radius,double reach,double seconds,
 double p0,double p1,double dwell_speed,const std::vector<double>& visibility,
 const std::vector<double>& permission){
 if(!std::isfinite(dwell_speed)||dwell_speed<0)throw std::invalid_argument("dwell speed");
 const auto delta=end-start;
 const double length=h::norm(delta);
 const auto dose=integrate({length,seconds,p0,p1,radius});
 auto e=swept_exposure(patch,seeds,start,end,radius,reach,seconds,1,1,visibility,permission);
 auto local=e.moving;
 const bool dwell=length==0&&(action==Action::Press||action==Action::Compact||action==Action::Compress);
 if(dwell)for(std::size_t i=0;i<local.size();++i)local[i]=e.time[i]*dwell_speed;
 auto allowed=patch;
 auto deposit=native_deposit;
 if(deposit.size()!=local.size())throw std::invalid_argument("native deposit size");
 for(std::size_t i=0;i<local.size();++i){
  if(e.moving[i]==0&&e.time[i]==0){allowed.weight[i]=0;deposit[i]=0;}
 }
 const double pressure=.5*(std::clamp(p0,0.,1.)+std::clamp(p1,0.,1.));
 const double total=dwell?seconds*dwell_speed:length;
 (void)dose;
 return geometry_target(allowed,action,material,deposit,compaction,tool_axis,
                        start+delta*.5,total,pressure,radius,&local);
 }
}
