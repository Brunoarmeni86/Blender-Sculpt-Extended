/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "clay_surface_solver.hh"
#include "material_transport_v21.hh"
#include <cstdlib>
#include <iostream>

using namespace sculpt_physics;
static void require(bool ok,const char *msg)
{
  if(!ok){std::cerr<<msg<<'\n';std::exit(EXIT_FAILURE);}
}
static double length(Vec3 p){return std::hypot(p.x,p.y,p.z);}
int main()
{
  // The host accepted these vertices. The legacy .5R depth cut discarded
  // the entire first sheet even though it is inside a native sphere.
  std::vector<ClaySurfaceVertex> v={
      {{-.15,-.15,.70},1},{{.15,-.15,.70},.5},
      {{.15,.15,.70},.25},{{-.15,.15,.70},0},
      {{-.15,-.15,.85},1},{{.15,-.15,.85},1},{{0,.15,.85},1}};
  const std::vector<ClaySurfaceTriangle> t={{0,1,2},{0,2,3},{4,5,6}};
  ClaySurfaceDab dab;
  dab.action=ClaySurfaceAction::Buildup;
  dab.radius=1;dab.dose=.05;dab.intensity=.5;
  const auto old=solve_clay_surface(v,t,dab);
  require(old.active==0,"legacy depth fixture");
  dab.sculpt_footprint_v33=true;
  const auto out=solve_clay_surface(v,t,dab);
  require(out.active==3,"native positive-factor vertices must be reachable");
  require(out.displacement[0].z>out.displacement[1].z &&
          out.displacement[1].z>out.displacement[2].z &&
          out.displacement[2].z>0,"native influence must retain its ordering");
  for(std::size_t i=3;i<v.size();++i)
    require(length(out.displacement[i])==0,"fixed halo and detached sheet cannot move");
  auto integrated=solve_material_transport_v21(v,t,dab,4);
  for(std::size_t i=3;i<v.size();++i)
    require(length(integrated.surface.displacement[i])==0,"substep leaked outside active patch");
  require(integrated.surface.displacement[0].z>0,"substep buildup must move");
  dab.intensity=0;
  const auto zero=solve_clay_surface(v,t,dab);
  for(const auto &d:zero.displacement)require(length(d)==0,"zero pressure must not move");
  // Topology selection may reject a detached patch but must not add an
  // extra attenuation to host factors on the supported connected patch.
  std::vector<Vec3> p={{0,0,0},{.1,0,0},{.2,0,0},{.3,0,0},{.4,0,0}};
  std::vector<Vec3> dual(p.size(),{0,0,1});
  ClaySurfaceEdges edges={{0,1},{1,2},{2,3},{3,4}};
  std::vector<double> w={1,.5,.1,0,1};
  std::size_t rejected=0;
  connected_contact_v31d2(p,dual,edges,{0,0,1},0,0,1.5,1,.65,w,rejected,true);
  require(w[0]==1 && w[1]==.5 && w[2]==.1,"host factors must not be multiplied again");
  require(w[3]==0 && w[4]==0,"zero-factor vertex cannot bridge material");
  require(rejected==1,"detached writable component reported");
  std::cout<<"V33 footprint tests passed\n";
  return EXIT_SUCCESS;
}
