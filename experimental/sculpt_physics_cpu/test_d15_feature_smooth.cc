/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d15_feature_smooth.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics::feature_smooth_d15;
static void check(bool b,const char* s){if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::abort();}}
static bool close(V a,V b){return length(sub(a,b))<1e-12;}
int main(){
 Settings k;Result r;
 std::vector<Vertex> v{{{0,0,0},{0,0,1},1.},{{-2,0,0},{0,0,1},0.},
                       {{1,0,0},{0,0,1},0.},{{0,1,1},{0,0,1},0.}};
 std::vector<Edge> e{{0,1,Sharp},{0,2,Sharp},{0,3,0}};
 check(evaluate(v,e,k,r)&&close(r.motion[0],{-.25,0,0})&&r.sliding==1,"crease slide and cap");
 for(std::size_t i=1;i<v.size();++i)check(r.motion[i]==V{},"read-only mask/halo");
 auto expected=r.motion;
 std::reverse(e.begin(),e.end());for(auto& x:e)std::swap(x.a,x.b);
 check(evaluate(v,e,k,r)&&r.motion==expected,"order independence");
 for(auto& x:v)x.co=add(x.co,{100,-8,2});
 check(evaluate(v,e,k,r)&&r.motion==expected,"translation invariance");
 v[0].weight=.5;check(evaluate(v,e,k,r)&&close(r.motion[0],{-.125,0,0}),"weight once");v[0].weight=1.;
 e={{0,1,Seam},{0,2,Seam},{0,3,0}};
 check(evaluate(v,e,k,r)&&close(r.motion[0],{-.25,0,0}),"UV seam follows its own edges");
 e[2].flags=FaceSet;check(evaluate(v,e,k,r)&&r.motion[0]==V{}&&r.pinned==1,"mixed junction pin");
 e={{0,1,Sharp},{0,2,FaceSet}};check(evaluate(v,e,k,r)&&r.motion[0]==V{},"mixed pair pin");
 e={{0,1,Sharp},{0,3,0}};check(evaluate(v,e,k,r)&&r.motion[0]==V{},"endpoint pin");
 e={{0,1,Sharp},{0,2,Sharp}};v[2].visible=false;
 check(evaluate(v,e,k,r)&&r.motion[0]==V{},"hidden feature pins");v[2].visible=true;
 e={{0,1,Sharp},{0,3,Sharp}};check(evaluate(v,e,k,r)&&r.motion[0]==V{},"bend pin");
 v={{{0,0,0},{0,0,1},1.},{{1,0,1},{0,0,1},0.},{{-1,0,1},{0,0,1},0.},{{0,0,.001},{0,0,-1},0.}};
 e={{0,1,0},{0,2,0}};
 check(evaluate(v,e,k,r)&&close(r.motion[0],{0,0,.2}),"interior normal attenuation and disconnected sheet");
 k.normal_keep=0.;check(evaluate(v,e,k,r)&&r.motion[0]==V{},"tangent projection");
 v[0].visible=false;check(evaluate(v,e,k,r)&&r.motion[0]==V{},"hidden active");v[0].visible=true;
 v[0].normal={};check(evaluate(v,e,k,r)&&r.motion[0]==V{},"degenerate normal");
 v[0].normal={0,0,1};k.normal_keep=.2;
 check(evaluate(v,e,k,r),"valid before invalid");expected=r.motion;
 e.push_back({1,0,0});check(!evaluate(v,e,k,r)&&r.motion==expected,"duplicate rejection transactional");
 e={{0,9,0}};check(!evaluate(v,e,k,r)&&r.motion==expected,"bad edge transactional");
 v[0].weight=std::numeric_limits<double>::quiet_NaN();e={{0,1,0}};
 check(!evaluate(v,e,k,r)&&r.motion==expected,"NaN rejection transactional");
 std::puts("D15_FEATURE_SMOOTH_REFERENCE_OK");
}
