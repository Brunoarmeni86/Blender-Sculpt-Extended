#include "artistic_material_v31d1110.hh"
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
 bool rejected=false;
 try{evaluate(Material::Mud,std::numeric_limits<double>::quiet_NaN(),0,1);}
 catch(const std::invalid_argument&){rejected=true;}
 check(rejected);
}
