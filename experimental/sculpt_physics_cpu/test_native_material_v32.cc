/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_material_v32.hh"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace sculpt_physics;

static void require(bool ok,const char *message)
{
  if(!ok){
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
static double norm(Vec3 v){return std::hypot(v.x,v.y,v.z);}
static bool same(Vec3 a,Vec3 b){return norm(a-b)<1e-12;}

int main()
{
  for(const auto family:{"Clay","Wax","Stone","Wood","Flesh","Mud","Fabric","Foam"}){
    const auto p=native_material_v32(family);
    for(int action=0;action<=14;++action){
      require(native_gain_v32(p,action,0)==1.0,"zero blend must equal native");
      const double gain=native_gain_v32(p,action,1.0);
      require(gain>=.75 && gain<=1.08,"native gain bound");
    }
  }
  const std::vector<Vec3> positions={{0,0,0},{.25,0,0},{.5,0,0},{.75,0,0},{1,0,0}};
  const std::vector<double> areas={1,2,1,1,-1000};
  const std::vector<std::array<std::size_t,2>> edges={{0,1},{1,2},{1,3},{1,4}};
  const std::vector<Vec3> initial={{0,0,1},{0,0,3},{0,0,0},{0,0,-2},{0,0,4}};
  auto goals=initial;
  const auto result=redistribute_native_v32(positions,areas,edges,{1,0,0},2,.3,.5,goals);
  require(result.links==1 && result.correction_scale>0,"only positive same-sign link must exchange");
  require(result.relative_budget_error<1e-8,"positive projected motion budget");
  require(goals[0].z>initial[0].z && goals[1].z<initial[1].z,"expected exchange direction");
  require(same(goals[0]*areas[0]+goals[1]*areas[1],
               initial[0]*areas[0]+initial[1]*areas[1]),"integrated vector conservation");
  for(std::size_t i=0;i<goals.size();++i){
    require(norm(goals[i]-initial[i])<=.3*norm(initial[i])+1e-12,"correction cap");
  }
  require(same(goals[2],initial[2]),"zero target unchanged");
  require(same(goals[3],initial[3]),"opposite target unchanged");
  require(same(goals[4],initial[4]),"negative projected area unchanged");
  auto check_rejected=[&](const std::vector<double> &a,
                          const std::vector<std::array<std::size_t,2>> &e,double amount){
    auto trial=initial;
    const auto rejected=redistribute_native_v32(positions,a,e,{1,0,0},2,amount,.5,trial);
    require(rejected.correction_scale==0,"invalid or disabled exchange must not apply");
    for(std::size_t i=0;i<trial.size();++i)require(same(trial[i],initial[i]),"rejection must be atomic");
  };
  check_rejected(areas,edges,0);
  check_rejected({1,2},edges,.3);
  check_rejected(areas,{{0,1},{1,99}},.3);
  check_rejected(areas,edges,std::numeric_limits<double>::quiet_NaN());
  auto bad_areas=areas;
  bad_areas[1]=std::numeric_limits<double>::infinity();
  check_rejected(bad_areas,edges,.3);
  std::cout << "V32 material tests passed\n";
  return EXIT_SUCCESS;
}
