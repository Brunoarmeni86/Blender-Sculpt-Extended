#include "hybrid_foundation_v31d11.hh"
#include <cstdlib>
#include <iostream>
using namespace sculpt_physics;
namespace h=hybrid_v31d11;
#define check(x) do { if(!(x)) { std::cerr << "failure at line " << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while(false)
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
 // Reindex an irregular patch, preserving triangle winding and physical inputs.
 std::vector<Vec3> q{{0,0,0},{.3,0,0},{0,1,0},{1,1,.1}};
 std::vector<h::Tri> qt{{0,1,2},{1,3,2}};
 std::vector<double> w{1,.7,.3,.1};
 auto original=h::make_patch(q,qt,w);
 std::vector<std::size_t> perm{2,0,3,1};
 auto qp=q;auto wp=w;auto tp=qt;
 for(std::size_t i=0;i<q.size();++i){qp[perm[i]]=q[i];wp[perm[i]]=w[i];}
 for(auto& tri:tp)for(auto& i:tri)i=perm[i];
 auto reordered=h::make_patch(qp,tp,wp);
 h::contact(original,0,4);h::contact(reordered,perm[0],4);
 auto fa=h::flux(original,original.weight,1);
 auto fb=h::flux(reordered,reordered.weight,1);
 for(std::size_t i=0;i<q.size();++i)check(std::abs(fa[i]-fb[perm[i]])<1e-12);
 for(auto tool:{h::Tool::Deposit,h::Tool::Press,h::Tool::Compact,h::Tool::Compress,h::Tool::Scrape,h::Tool::Push,h::Tool::Smear,h::Tool::Melt,h::Tool::Grab}){
  auto ua=h::optimize(original,h::target(original,tool,{},Vec3{1,0,0},1,.05));
  auto ub=h::optimize(reordered,h::target(reordered,tool,{},Vec3{1,0,0},1,.05));
  for(std::size_t i=0;i<q.size();++i)check(h::norm(ua[i]-ub[perm[i]])<1e-10);
 }
 std::cout<<"hybrid foundation tests passed\n";
}
