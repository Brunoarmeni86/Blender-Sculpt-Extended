/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#include <cstdlib>
#include <iostream>
using namespace sculpt_physics;
static void require(bool ok,const char *msg)
{
  if(!ok){std::cerr<<msg<<'\n';std::exit(EXIT_FAILURE);}
}
int main()
{
  constexpr int side=13;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  for(int y=0;y<side;++y)for(int x=0;x<side;++x){
    const double px=.12*(x-6),py=.12*(y-6);
    v.push_back({{px,py,0},(px*px+py*py<.60)?1.:0.});
  }
  for(int y=0;y<side-1;++y)for(int x=0;x<side-1;++x){
    const auto a=std::size_t(y*side+x),b=a+1,c=a+side,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  const auto other=v.size();
  v.push_back({{.02,.03,.02},1});
  v.push_back({{.09,.03,.02},1});
  v.push_back({{.02,.10,.02},1});
  t.push_back({other,other+1,other+2});
  ClaySurfaceDab dab;
  dab.action=ClaySurfaceAction::Scrape;
  dab.radius=1;dab.dose=.05;dab.intensity=1;
  dab.extended_material=true;dab.density_independent=true;
  dab.shape_transport=true;dab.tool_v25=true;dab.tool_v26=true;
  dab.stable_v28=true;dab.refine_v28b=true;dab.live_surface_v30=true;
  dab.contact_v30b=true;dab.adaptive_v31=true;dab.average_surface_v31d=.65;
  dab.continuous_contact_v31d2=true;dab.gradient_v31c=true;
  const auto a=solve_material_transport_v21(v,t,dab,4,false).surface;
  dab.coverage_v31d9=true;
  const auto b=solve_material_transport_v21(v,t,dab,4,false).surface;
  require(a.coverage_weights_v31d9.empty(),"diagnostic off allocates no weights");
  require(b.coverage_weights_v31d9.size()==v.size(),"coverage contains all local IDs");
  require(b.coverage_weights_v31d9[6*side+6]>0,"center selected");
  for(std::size_t i=0;i<v.size();++i){
    const auto d=a.displacement[i]-b.displacement[i];
    require(std::hypot(d.x,d.y,d.z)==0,"diagnostic must not change solver result");
    if(v[i].influence==0 || i>=other)
      require(b.coverage_weights_v31d9[i]==0,"masked/disconnected not selected");
  }
  std::cout<<"D9 coverage tests passed\n";
}
