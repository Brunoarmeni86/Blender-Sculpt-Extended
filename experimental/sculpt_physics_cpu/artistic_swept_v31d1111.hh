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
}
