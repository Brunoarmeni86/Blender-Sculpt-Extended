#include "artistic_geometry_v31d1110.hh"
#include "artistic_swept_v31d1111.hh"
#include "artistic_surface_v31d1111.hh"
#include <cstdlib>
#include <limits>
using namespace sculpt_physics::artistic_v31d1110;
static void check(bool b){if(!b)std::abort();}
int main(){
 auto soft=evaluate(Material::SoftClay,1,0,1);
 auto firm=evaluate(Material::FirmClay,1,0,1);
 auto mud=evaluate(Material::Mud,1,0,1);
 auto wax=evaluate(Material::Wax,1,0,1);
 auto foam=evaluate(Material::Foam,1,0,1);
 auto flesh=evaluate(Material::Flesh,1,0,1);
 check(soft.deposit==firm.deposit && soft.transport>firm.transport);
 check(mud.transport>wax.transport);
 check(foam.elastic_sink>flesh.elastic_sink && flesh.elastic_sink>wax.elastic_sink);
 check(evaluate(Material::Putty,1,0,1).drag>soft.drag);
 check(evaluate(Material::Wood,1,0,1).drag>evaluate(Material::Wood,1,0,0).drag);
 check(evaluate(Material::FirmClay,1,1,1).plastic_sink<firm.plastic_sink);
 for(int i=0;i<9;++i){
  auto z=evaluate(static_cast<Material>(i),0,0,1);
  check(z.deposit==0 && z.transport==0 && z.drag==0 && z.sink==0);
 }
 Intent i{.2,.1,.5,0,1};
 auto build=target(Action::Build,Material::FirmClay,i);
 check(build.normal_deposit==i.native_deposit);
 auto push=target(Action::Push,Material::Mud,i);
 auto smear=target(Action::Smear,Material::Mud,i);
 check(push.transport>smear.transport);
 auto press=target(Action::Press,Material::Foam,i);
 auto compact=target(Action::Compact,Material::Foam,i);
 check(compact.axial_sink>press.axial_sink && compact.transport<press.transport);
 check(target(Action::Gather,Material::Putty,i).radial_gather>0);
 check(target(Action::Melt,Material::Mud,i).relaxation>0);
 for(int n: {1,2,8,64,256}){
  double moving=0,time=0;
  for(int j=0;j<n;++j){
   auto d=integrate({1./n,2./n,.2+.6*j/n,.2+.6*(j+1)/n,.1});
   moving+=d.moving;time+=d.stationary;
  }
  check(std::abs(moving-.5)<1e-12 && std::abs(time-1.)<1e-12);
 }
 auto stationary=integrate({0,1,.5,.5,.1});
 check(stationary.moving==0 && stationary.stationary==.5);
 auto patch=h::make_patch({{0,0,0},{1,0,0},{0,1,0}},{{0,1,2}},{1,1,0});
 for(int a=0;a<8;++a)for(int m=0;m<9;++m){
  auto u=geometry_target(patch,static_cast<Action>(a),static_cast<Material>(m),
    {.01,.01,0},{0,0,0},{1,0,1},{.3,.3,0},.01,1);
  check(u.size()==3 && h::norm(u[2])==0);
  for(auto v:u)check(std::isfinite(h::norm(v)));
 }
 auto u=geometry_target(patch,Action::Build,Material::FirmClay,
    {.01,.01,0},{0,0,0},{1,0,1},{.3,.3,0},.01,1);
 check(u[0].z>0);
 auto flux=pair_transport(patch,{1,0,0},4,.1);
 check(flux[0]<0 && flux[1]>0 && flux[2]==0);
 double balance=0;
 for(std::size_t k=0;k<flux.size();++k)balance+=flux[k]*patch.area[k];
 check(std::abs(balance)<1e-12);
 auto uniform=pair_transport(patch,{1,1,0},4,.1);
 for(auto x:uniform)check(x==0);
 auto transported=geometry_target(patch,Action::Press,Material::Mud,
    {0,0,0},{0,.8,0},{0,0,1},{.3,.3,0},.01,1,4);
 check(h::norm(transported[2])==0);
 for(auto v:transported)check(std::isfinite(h::norm(v)));
 auto sweep=swept_exposure(patch,{0,1},{0,0,0},{1,0,0},.2,2,1,1,1,
    {1,1,1},{1,1,0});
 check(sweep.moving[0]>0 && sweep.moving[1]>0 && sweep.moving[2]==0);
 auto dwell=swept_exposure(patch,{0},{0,0,0},{0,0,0},.2,2,2,.5,.5,
    {1,1,1},{1,1,0});
 check(dwell.moving[0]==0 && std::abs(dwell.time[0]-1)<1e-12);
 auto blocked=swept_exposure(patch,{0,1},{0,0,0},{1,0,0},.2,2,1,1,1,
    {1,0,1},{1,1,0});
 check(blocked.moving[1]==0);
 auto plane=capture_plane({0,0,-.1},{0,0,1});
 auto scrape=scrape_plane(patch,plane,Material::Mud,{.1,.1,.1},1,1);
 check(scrape[0].z<0 && h::norm(scrape[2])==0);
 auto above=capture_plane({0,0,.1},{0,0,1});
 auto no_cut=scrape_plane(patch,above,Material::Mud,{.1,.1,.1},1,1);
 for(auto v:no_cut)check(h::norm(v)==0);
 check(sculptgl_falloff(0)==1 && sculptgl_falloff(1)==0);
 check(std::abs(sculptgl_falloff(.5)-.6875)<1e-12);
 for(int a=0;a<6;++a)for(int m=0;m<9;++m){
  auto v=surface_target(patch,static_cast<SurfaceAction>(a),static_cast<Material>(m),
    capture_plane({0,0,0},{0,0,1}),{.3,.3,0},{1,0,0},{.1,.1,.1},1,1);
  check(v.size()==3 && h::norm(v[2])==0);
  for(auto x:v)check(std::isfinite(h::norm(x)));
 }
 auto clay=surface_target(patch,SurfaceAction::Clay,Material::FirmClay,
    capture_plane({0,0,0},{0,0,1}),{0,0,0},{1,0,0},{.1,.1,0},1,1);
 check(clay[0].z>0 && clay[0].z<=.1);
 bool rejected=false;
 try{evaluate(Material::Mud,std::numeric_limits<double>::quiet_NaN(),0,1);}
 catch(const std::invalid_argument&){rejected=true;}
 check(rejected);
}
