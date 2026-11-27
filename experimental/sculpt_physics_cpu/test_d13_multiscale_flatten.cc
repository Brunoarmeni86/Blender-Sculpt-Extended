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
 std::puts("D13_MULTISCALE_FLATTEN_OK");
}
