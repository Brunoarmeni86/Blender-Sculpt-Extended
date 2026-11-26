#include "artistic_tool_v31d1110.hh"
#include "artistic_stroke_v31d1111.hh"
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
 bool rejected=false;
 try{evaluate(Material::Mud,std::numeric_limits<double>::quiet_NaN(),0,1);}
 catch(const std::invalid_argument&){rejected=true;}
 check(rejected);
}
