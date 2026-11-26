#include "hybrid_foundation_v31d11.hh"
#include <cstdlib>
#include <iostream>
using namespace sculpt_physics;
namespace h=hybrid_v31d11;
void check(bool x){if(!x)std::exit(1);}
int main(){
 std::vector<Vec3> p{{0,0,0},{1,0,0},{0,1,0},{1,1,0},{3,0,0},{4,0,0},{3,1,0}};
 std::vector<h::Tri> t{{0,1,2},{1,3,2},{4,5,6}};
 auto s=h::make_patch(p,t,{1,.5,.5,0,1,1,1});
 h::contact(s,0,3);
 check(s.weight[3]==0&&s.weight[4]==0);
 auto f=h::flux(s,s.weight,1);double sum=0;
 for(std::size_t i=0;i<f.size();++i)sum+=f[i]*s.area[i];
 check(std::abs(sum)<1e-12&&f[0]<0&&f[3]==0);
 for(auto tool:{h::Tool::Deposit,h::Tool::Press,h::Tool::Compact,h::Tool::Compress,h::Tool::Scrape,h::Tool::Push,h::Tool::Smear,h::Tool::Melt,h::Tool::Grab}){
 auto u=h::optimize(s,h::target(s,tool,{},Vec3{1,0,0},1,.05));
 check(h::norm(u[3])==0&&h::norm(u[4])==0);
 check(gradient_motion_scale_v31c(p,t,u)>=1-1e-10);
 }
 auto uniform=h::make_patch(p,t,{1,1,1,1,0,0,0});
 auto press=h::target(uniform,h::Tool::Press,{},Vec3{0,0,1},1,.05);
 check(press[0].z<0);
 auto u=h::optimize(uniform,press,true);
 check(u[0].z<0&&h::norm(u[4])==0);
 auto v=h::flux(uniform,uniform.weight,1);
 for(double x:v)check(std::abs(x)<1e-12);
 // Zero-net transport must remain conservative after optimization.
 std::vector<Vec3> desired(p.size());
 for(std::size_t i=0;i<p.size();++i)desired[i]=s.normal[i]*f[i];
 auto balanced=h::optimize(s,desired,true);
 double net=0, magnitude=0;
 for(std::size_t i=0;i<p.size();++i){
  double x=s.area[i]*h::dot(s.normal[i],balanced[i]);
  net+=x;magnitude+=std::abs(x);
 }
 check(std::abs(net)<=1e-12+1e-9*magnitude);
 check(gradient_motion_scale_v31c(p,t,balanced)>=1-1e-10);
 // Nonfinite inputs must fail explicitly.
 bool rejected=false;
 try{h::flux(s,s.weight,std::numeric_limits<double>::infinity());}
 catch(const std::invalid_argument&){rejected=true;}
 check(rejected);
 rejected=false;
 desired[0].x=std::numeric_limits<double>::quiet_NaN();
 try{h::optimize(s,desired);}
 catch(const std::invalid_argument&){rejected=true;}
 check(rejected);
 std::cout<<"hybrid foundation tests passed\n";
}
