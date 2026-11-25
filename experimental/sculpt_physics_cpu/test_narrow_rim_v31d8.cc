/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#include <cstdlib>
#include <iostream>

using namespace sculpt_physics;
static void require(bool ok,const char *msg)
{
  if(!ok){std::cerr<<msg<<'\n';std::exit(EXIT_FAILURE);}
}
static double length(Vec3 v){return std::hypot(v.x,v.y,v.z);}
static double peak(const ClaySurfaceResult &r)
{
  double p=0;
  for(const auto &m:r.displacement)p=std::max(p,length(m));
  return p;
}
int main()
{
  require(contact_rim_radius_v31d8(.04,false)==.25,"old minimum");
  require(contact_rim_radius_v31d8(.04,true)<.06,"small support gets smaller rim");
  require(contact_rim_radius_v31d8(.5,true)==contact_rim_radius_v31d8(.5,false),"ordinary support unchanged");
  constexpr int side=9;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  for(int y=0;y<side;++y)for(int x=0;x<side;++x){
    const double px=.008*(x-4),py=.008*(y-4);
    v.push_back({{px,py,0},1});
  }
  for(int y=0;y<side-1;++y)for(int x=0;x<side-1;++x){
    const auto a=std::size_t(y*side+x),b=a+1,c=a+side,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  const auto other=v.size();
  v.push_back({{.009,.009,.003},1});
  v.push_back({{.016,.009,.003},1});
  v.push_back({{.009,.016,.003},1});
  t.push_back({other,other+1,other+2});
  v[0].influence=0;
  ClaySurfaceDab dab;
  dab.action=ClaySurfaceAction::Press;
  dab.radius=1;dab.dose=.02;dab.intensity=1;
  dab.extended_material=true;dab.density_independent=true;
  dab.shape_transport=true;dab.tool_v25=true;dab.tool_v26=true;
  dab.stable_v28=true;dab.refine_v28b=true;dab.live_surface_v30=true;
  dab.contact_v27b=true;dab.contact_v30b=true;dab.adaptive_v31=true;
  dab.average_surface_v31d=.65;dab.continuous_contact_v31d2=true;
  dab.gradient_v31c=true;
  const auto old=solve_clay_surface(v,t,dab);
  require(old.rim_empty_v31d8==1 && old.rim_acceptance_v31d8==0 && peak(old)==0,
          "original rim has no receiver on tiny patch");
  dab.narrow_rim_v31d8=true;
  const auto repaired=solve_clay_surface(v,t,dab);
  require(repaired.rim_adapted_v31d8==1 && repaired.rim_receiver_v31d8>0 &&
          repaired.rim_acceptance_v31d8>0 && peak(repaired)>1e-12,
          "smaller internal receiving profile restores nonzero motion");
  require(length(repaired.displacement[0])==0,"mask unchanged");
  for(std::size_t i=other;i<v.size();++i)
    require(length(repaired.displacement[i])==0,"disconnected sheet unchanged");
  require(std::abs(repaired.signed_swept_volume)<1e-10,"first-order signed budget");
  auto big=v;big.resize(other);
  auto bt=t;bt.pop_back();
  for(auto &a:big)a.position=a.position*10.;
  const auto enabled=solve_clay_surface(big,bt,dab);
  dab.narrow_rim_v31d8=false;
  const auto disabled=solve_clay_surface(big,bt,dab);
  require(enabled.displacement.size()==disabled.displacement.size(),"same broad patch size");
  for(std::size_t i=0;i<big.size();++i)
    require(length(enabled.displacement[i]-disabled.displacement[i])==0,
            "ordinary broad patch exactly unchanged");
  dab.narrow_rim_v31d8=true;
  const auto integrated=solve_material_transport_v21(v,t,dab,4,false);
  require(integrated.surface.rim_samples_v31d8>0 && integrated.surface.rim_adapted_v31d8>0,
          "integrator retains diagnostic stats");
  std::cout<<"D8 narrow support tests passed\n";
  return EXIT_SUCCESS;
}
