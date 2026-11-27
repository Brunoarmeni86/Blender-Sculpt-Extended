/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d13_multiscale_flatten.hh"
#include <cstdlib>
#include <cstdio>
namespace f=sculpt_physics::dedicated_d13::flatten;
namespace ms=f::ms;
static void check(bool b){if(!b)std::abort();}
int main(){
 ms::Chart c;
 c.points={{0.,0.,.04,1.,1.},{0.,0.,-.04,1.,1.},{0.,0.,.04,1.,0.},
           {0.,0.,.04,1.,.5},{0.,0.,0.,1.,1.}};
 std::vector<double> d={-.01,.01,0.,-.005,0.},out;
 f::Params p;
 check(f::evaluate(c,d,p,out));
 check(out[0]<d[0]&&out[1]>d[1]&&out[2]==0.&&out[4]==0.);
 check(std::abs(out[3]-.5*out[0])<1e-12);
 p.mode=f::Mode::Hard;check(f::evaluate(c,d,p,out));
 check(std::abs(out[0]+.034)<1e-12&&std::abs(out[1]-.034)<1e-12);
 p.mode=f::Mode::Scrape;check(f::evaluate(c,d,p,out));
 check(out[0]<0.&&out[1]==0.&&out[2]==0.);
 p.mode=f::Mode::Fill;check(f::evaluate(c,d,p,out));
 check(out[0]==0.&&out[1]>0.&&out[2]==0.);
 p.mode=f::Mode::Hard;p.offset=.02;check(f::evaluate(c,d,p,out));
 check(out[4]>0.);
 p.offset=1.;check(!f::evaluate(c,d,p,out));
 check(f::parse("invalid")==f::Mode::Invalid);
 double value=0.;
 check(f::number(".12",value,-.25,.25)&&std::abs(value-.12)<1e-12);
 check(!f::number("nan",value,-.25,.25)&&!f::number("1x",value,-.25,.25));
 // V2 fixed plane survives a changed local fit; no opposite-side motion.
 ms::Chart v2;v2.axis={0.,0.,1.};v2.plane_normal=v2.axis;v2.radius=1.;
 v2.points={{0.,0.,0.,1.,1.},{0.,0.,0.,1.,1.},{0.,0.,0.,1.,0.},
            {0.,0.,0.,1.,1.},{0.,0.,0.,1.,1.}};
 std::vector<ms::Sample> samples={
  {{0.,0.,.04},{0.,0.,1.},1.,1.},{{0.,0.,-.04},{0.,0.,1.},1.,1.},
  {{0.,0.,.04},{0.,0.,1.},0.,1.},{{.99,0.,.04},{0.,0.,1.},1.,1.},
  {{1.01,0.,.04},{0.,0.,1.},1.,1.}};
 f::PlaneState state;p.mode=f::Mode::Hard;p.offset=0.;
 check(f::update_plane(v2,p.mode,0.,state));
 v2.plane_point={0.,0.,.12};
 check(f::update_plane(v2,p.mode,0.,state)&&state.point.z==0.);
 check(f::evaluate_v2(v2,samples,{},p,state,.65,out));
 check(std::abs(out[0]+.034)<1e-12&&std::abs(out[1]-.034)<1e-12&&out[2]==0.);
 check(std::abs(out[3])<.001&&out[4]==0.);
 check(f::update_plane(v2,f::Mode::Scrape,0.,state));
 p.mode=f::Mode::Scrape;state.point={0.,0.,0.};
 check(f::evaluate_v2(v2,samples,{},p,state,.65,out)&&out[0]<0.&&out[1]==0.);
 p.mode=f::Mode::Adaptive;state={};
 check(f::update_plane(v2,p.mode,.35,state));
 const double old=state.point.z;v2.plane_point.z+=.1;
 check(f::update_plane(v2,p.mode,.35,state)&&state.point.z==old);
 v2.center.x=.2;
 check(f::update_plane(v2,p.mode,.35,state)&&state.point.z>old&&state.point.z<v2.plane_point.z);
 std::puts("D13_MULTISCALE_FLATTEN_OK");
}
