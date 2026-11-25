/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stroke_context_v27.hh"
#include <cstdlib>
#include <iostream>
using namespace sculpt_physics;
static void require(bool ok,const char *message){
  if(!ok){std::cerr<<message<<'\n';std::exit(EXIT_FAILURE);}
}
int main(){
  require(continuous_exposure_v31d5(.15,false,false,.20)==.15,"first contact unchanged");
  require(continuous_exposure_v31d5(.2,true,false,0)==.2,"off unchanged");
  require(continuous_exposure_v31d5(0,true,false,.20)==0,"no stationary dose");
  require(continuous_exposure_v31d5(.8,true,true,.20)==.8,"no bonus across gaps");
  require(continuous_exposure_v31d5(.9,true,false,.5)==1,"dose bounded");
  const auto sum=[](int parts){
    double result=0;
    for(int i=0;i<parts;++i)result+=continuous_exposure_v31d5(.6/parts,true,false,.2);
    return result;
  };
  require(std::abs(sum(3)-sum(30))<1e-12,"distance normalized independent of partition");
  require(std::abs(sum(10)-.72)<1e-12,"bounded added exposure");
  bool rejected=false;
  try{continuous_exposure_v31d5(.1,true,false,.6);}catch(const std::invalid_argument &){rejected=true;}
  require(rejected,"invalid bonus rejected");
  std::cout<<"V31D5 continuous exposure tests passed\n";
}
