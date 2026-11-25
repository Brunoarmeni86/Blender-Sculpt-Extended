/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "stroke_context_v27.hh"
#include "tangential_transport_v31d6.hh"
#include "material_transport_v21.hh"
#include <cstdlib>
#include <iostream>

using namespace sculpt_physics;
static void require(bool ok,const char *msg)
{
  if(!ok){std::cerr<<msg<<'\n';std::exit(EXIT_FAILURE);}
}
static bool near(double a,double b){return std::abs(a-b)<1e-12;}
static double length(Vec3 p){return std::hypot(p.x,p.y,p.z);}

int main()
{
  require(near(continuous_dose_v31d5(.15,false,false,true,.8,1.3),.15),"first dab unchanged");
  require(continuous_dose_v31d5(0,true,false,true,1,1.3)==0,"stationary space no new dose");
  require(near(continuous_dose_v31d5(.4,true,true,true,1,1.3),.4),"gap unchanged");
  require(near(continuous_dose_v31d5(.2,true,false,false,1,1.3),.26),"spatial gain");
  require(near(continuous_dose_v31d5(.1,true,false,true,.8,1),.16),"native uniform ratio");
  require(near(continuous_dose_v31d5(.1,true,false,true,0,1),.1),"invalid native ratio fallback");
  double sum=0;
  for(int i=0;i<10;++i)sum+=continuous_dose_v31d5(.02,true,false,false,0,1.3);
  require(near(sum,.26),"gain independent of event count below clamp");
  StrokeContextV27 before;
  before.started=true;before.radius=1;before.pressure=.7;
  before.center={0,0,0};before.axis={0,0,1};before.normalized_travel=.45;
  before.tool_plane_valid=true;before.transport_load_v30b=.65;
  before.transport_direction_v30b={0,1,0};
  auto step=stage_stroke_v27(before,{.10,0,0},{0,0,1},1,.7);
  require(refresh_contact_v31d5(step,before,.5),"spatial refresh");
  require(!step.next.tool_plane_valid && near(step.dose,.1) &&
          near(step.next.transport_load_v30b,.65),"refresh does not add dose or reset load");
  refresh_transport_direction_v31d5(step);
  require(near(step.next.transport_direction_v30b.x,1),"refresh aligns direction");
  require(before.tool_plane_valid,"staged refresh leaves committed state unchanged");
  auto later=stage_stroke_v27(step.next,{.15,0,0},{0,0,1},1,.7);
  require(!refresh_contact_v31d5(later,step.next,.5),"no duplicate refresh before next interval");

  // Flat connected fan plus fixed halo: tangent must not invent normal
  // motion or modify zero-weight vertices, even on a zero-height target.
  std::vector<ClaySurfaceVertex> v={
      {{0,0,0},1},{{-.5,-.5,0},1},{{.5,-.5,0},1},
      {{.5,.5,0},1},{{-.5,.5,0},0}};
  const std::vector<ClaySurfaceTriangle> t={{0,1,2},{0,2,3},{0,3,4},{0,4,1}};
  const std::vector<double> weights={1,0,.5,.25,0};
  ClaySurfaceDab dab;
  dab.action=ClaySurfaceAction::Push;dab.radius=1;dab.dose=.1;
  dab.intensity=1;dab.direction={1,0,0};dab.gradient_v31c=true;
  std::vector<Vec3> motion(v.size());
  require(add_tangential_transport_v31d6(v,t,dab,weights,motion).moved==0,"disabled tangent no-op");
  dab.tangential_gain_v31d6=.18;
  const auto tangent=add_tangential_transport_v31d6(v,t,dab,weights,motion);
  require(tangent.moved==3 && tangent.scale>0 && tangent.peak>0,"tangent moves selected patch");
  require(near(tangent.signed_volume,0),"first-order tangent volume");
  require(motion[0].x>motion[2].x && motion[2].x>motion[3].x && motion[3].x>0,
          "selected weights preserved");
  require(length(motion[1])==0 && length(motion[4])==0,"rejected and fixed vertices remain fixed");
  for(auto d:motion)require(near(d.z,0) && length(d)<=.04,"flat tangent bounded");
  dab.sign=-1;
  std::vector<Vec3> inverse(v.size());
  require(add_tangential_transport_v31d6(v,t,dab,weights,inverse).moved>0 &&
          inverse[0].x<0,"reverse gesture");
  dab.dose=0;
  auto saved=inverse;
  require(add_tangential_transport_v31d6(v,t,dab,weights,inverse).moved==0,"zero dose no-op");
  for(std::size_t i=0;i<saved.size();++i)
    require(length(saved[i]-inverse[i])==0,"zero dose preserves baseline");
  std::cout<<"V31D5/D6 tests passed\n";
  return EXIT_SUCCESS;
}
