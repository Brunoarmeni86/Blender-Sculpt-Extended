/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "clay_surface_solver.hh"
#include <cstdlib>
#include <iostream>

using namespace sculpt_physics;
static void require(bool ok,const char *msg)
{
  if(!ok){std::cerr<<msg<<'\n';std::exit(EXIT_FAILURE);}
}

int main()
{
  // Winding graph within the original geometric candidate region.
  // Geodesic lengths from seed: 0, .45, .90, 1.35, 1.55, 1.75.
  const std::vector<Vec3> p={
      {0,0,0},{.45,0,0},{.45,.45,0},{0,.45,0},
      {0,.25,0},{-.20,.25,0},{0,.20,0},{0,.15,0},{.01,.01,0}};
  const std::vector<Vec3> dual(p.size(),{0,0,1});
  const ClaySurfaceEdges edges={{0,1},{1,2},{2,3},{3,4},{4,5},{4,6},{6,7}};
  const std::vector<double> initial={1,.8,.7,.5,.3,.2,0,.6,1};
  auto old=initial,disabled=initial,extended=initial;
  std::size_t a=0,b=0,c=0;
  connected_contact_v31d2(p,dual,edges,{0,0,1},0,0,1.5,1,.65,old,a);
  connected_contact_v31d2(p,dual,edges,{0,0,1},0,0,1.5,1,.65,disabled,b,false);
  connected_contact_v31d2(p,dual,edges,{0,0,1},0,0,1.5,1,.65,extended,c,true);
  require(old==disabled && a==b,"default and explicit off must agree");
  for(std::size_t i=0;i<3;++i)
    require(old[i]==extended[i],"inner plateau unchanged on uniform normals");
  require(old[3]>0 && extended[3]>old[3],"outer feather extended");
  require(old[4]==0 && extended[4]>0,"reachable outer point recovered");
  require(extended[5]==0,"reach extension remains bounded");
  require(extended[6]==0 && extended[7]==0,"zero-factor barrier preserved");
  require(extended[8]==0,"disconnected sheet remains excluded");
  require(a==c+1,"exactly one new reachable vertex");
  for(std::size_t i=0;i<p.size();++i)
    require(std::isfinite(extended[i]) && extended[i]>=0 && extended[i]<=initial[i],
            "no influence invented or amplified above host factor");
  std::cout<<"V31D4 contact reach tests passed\n";
  return EXIT_SUCCESS;
}
