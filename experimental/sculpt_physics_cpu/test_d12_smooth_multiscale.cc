/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d12_smooth_multiscale.hh"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace sculpt_physics;
using namespace sculpt_physics::dedicated_d12;
namespace ms=dedicated_d12::smooth_multiscale;
static void check(bool b,const char* message){
 if(!b){std::fprintf(stderr,"D12 Smooth FAIL: %s\n",message);std::abort();}
}
static std::vector<ms::Sample> fixture(int n,bool plane=false,double scale=1.){
 std::vector<ms::Sample> out;
 const double step=1.8/double(n-1);
 for(int j=0;j<n;++j)for(int i=0;i<n;++i){
  const double x=-.9+i*step,y=-.9+j*step;
  const double r=std::sqrt(x*x+y*y);
  if(r>=1.)continue;
  const double z=plane?.08*x-.05*y:.16*std::exp(-(x*x+y*y)/.045);
  const double dx=plane?.08:-2*x*z/.045,dy=plane?-.05:-2*y*z/.045;
  const double w=std::max(.0001,1.-r);
  out.push_back({{x*scale,y*scale,z*scale},unit({-dx,-dy,1}),w,step*step*scale*scale});
 }
 return out;
}
static double apex(const std::vector<ms::Sample>& samples,const std::vector<double>& delta){
 std::size_t best=0;
 for(std::size_t i=1;i<samples.size();++i)
  if(samples[i].p.z>samples[best].p.z)best=i;
 return delta[best];
}
int main(){
 auto plane=fixture(41,true);
 ms::Chart chart;
 check(ms::prepare(plane,{0,0,0},1.,chart),"tilted plane chart");
 auto d=ms::cpu(chart);
 check(ms::validate(chart,d),"plane finite bound");
 for(double h:d)check(std::abs(h)<1e-10,"affine plane invariant");
 double baseline=0.;
 for(int density:{31,61,121,241}){
  auto samples=fixture(density);
  const auto start=std::chrono::steady_clock::now();
  check(ms::prepare(samples,{0,0,0},1.,chart),"bump chart");
  d=ms::cpu(chart);
  check(ms::validate(chart,d),"finite bounded motion");
  const auto a=apex(samples,d);
  check(a<-.025,"broad relief must move substantially");
  if(baseline==0.)baseline=a;
  check(std::abs(a-baseline)<.006,"density invariance");
  double pos=0.,neg=0.;
  for(double v:d){pos=std::max(pos,v);neg=std::min(neg,v);}
  check(pos>0.001&&neg<-.025,"bump relaxation lowers and fills relief");
  std::printf("D12 Multiscale CPU points=%zu apex_R=%.6f ms=%.3f\n",samples.size(),a,
      std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
 }
 // Above fixtures deliberately reach the safety cap. Also verify the
 // unsaturated response so equality at the cap cannot hide density bias.
 double uncapped=0.;
 for(int density:{31,61,121,241}){
  auto samples=fixture(density);
  for(auto& q:samples){q.p.z*=.25;q.normal=unit({q.normal.x*.25,q.normal.y*.25,q.normal.z});}
  check(ms::prepare(samples,{0,0,0},1.,chart),"shallow bump chart");
  const auto a=apex(samples,ms::cpu(chart));
  check(a<-.006&&a>-.054,"unsaturated strong response");
  if(uncapped==0.)uncapped=a;
  check(std::abs(a-uncapped)<.003,"unsaturated density invariance");
  std::printf("D12 Multiscale shallow points=%zu apex_R=%.6f\n",samples.size(),a);
 }
 auto small=fixture(61,false,.001);
 check(ms::prepare(small,{0,0,0},.001,chart),"small object scale");
 check(std::abs(apex(small,ms::cpu(chart))-baseline)<.006,"object scale invariance");
 auto folded=fixture(31);
 folded.front().normal={0,0,-1};
 check(!ms::prepare(folded,{0,0,0},1.,chart),"reject folded sheet");
 auto bad=fixture(31);
 bad.front().p.z=2.;
 check(!ms::prepare(bad,{0,0,0},1.,chart),"reject excessive depth");
 auto masked=fixture(31);
 masked.front().weight=0.;
 check(!ms::prepare(masked,{0,0,0},1.,chart),"zero permission must be excluded by host");
 // Explicit output validation protects the native writer against invalid CUDA.
 check(ms::prepare(fixture(31),{0,0,0},1.,chart),"validation chart");
 d=ms::cpu(chart);d[0]=1.;
 check(!ms::validate(chart,d),"reject out-of-budget GPU motion");
}
