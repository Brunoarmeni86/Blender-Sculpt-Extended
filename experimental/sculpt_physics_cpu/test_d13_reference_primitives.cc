/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d13_reference_primitives.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics::dedicated_d13;
static void check(bool ok,const char* label){if(!ok){std::fprintf(stderr,"D13: %s\n",label);std::abort();}}
int main(){
 const Stroke s{{-.5,0,0},{.5,0,0},{0,0,1},{1,0,0},1.,.1};
 Sample p{{0,0,.2},{0,0,1},1.,1.,true};
 check(scrape(p,s,{0,0,0},{0,0,1}).z<0,"scrape high point");
 p.p.z=-.1;check(norm(scrape(p,s,{0,0,0},{0,0,1}))==0,"scrape never deposits");
 p.p.z=0;check(chisel(p,s,{0,0,0},{0,1,0},.2,.3).z<0,"incision");
 p.p.y=.3;check(norm(chisel(p,s,{0,0,0},{0,1,0},.2,.3))==0,"flat outside tip");
 p.p.y=0;p.writable=false;
 check(norm(chisel(p,s,{0,0,0},{0,1,0},.2,.3))==0,"mask");
 p.writable=true;p.n={0,0,-1};check(norm(chisel(p,s,{0,0,0},{0,1,0},.2,.3))==0,"back sheet");
 const std::vector<Cell> cells={{1.,1.,10.,true},{2.,1.,.25,true},{3.,1.,10.,true},{1.,1.,1.,false}};
 auto f=conservative_transfer(cells,{{0,1,1.},{0,2,1.},{0,3,1.}});
 double sum=0;for(double v:f.volume)sum+=v;
 check(std::abs(sum)<1e-12,"conservation");
 check(f.volume[0]>=-1.&&f.volume[1]<=.25&&f.volume[3]==0.,"budget and pins");
 check(std::abs(f.height[1]*2.-f.volume[1])<1e-12,"area normalization");
 check(mud_flux(.2,.3,1.,1.,1.)==0.,"mud yield");
 check(mud_flux(-.5,.3,1.,1.,1.)<0.,"mud signed");
 check(push_flux(1.,.2,1.,1.)>0.,"push direction");
 check(gather_flux(1.,2.,1.,.2,1.,1.)>0.&&gather_flux(1.,1.,2.,.2,1.,1.)<0.,"gather convergence");
}
