#include "hybrid_foundation_v31d11.hh"
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <locale>
using namespace sculpt_physics;
namespace h=hybrid_v31d11;
#define check(x) do { if(!(x)) { std::cerr << "failure at line " << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while(false)
int main(){
 std::cout.imbue(std::locale::classic());
 std::cout<<std::scientific<<std::setprecision(17);
 std::cout<<"metric,case,value\n";
 auto metric=[](const char* name,int id,double value){std::cout<<name<<","<<id<<","<<value<<"\n";};
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
 metric("transport_net",0,sum);metric("optimized_net",0,net);metric("optimized_absolute_proxy",0,magnitude);
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
 double flux_max=0;
 for(std::size_t i=0;i<q.size();++i){double e=std::abs(fa[i]-fb[perm[i]]);flux_max=std::max(flux_max,e);check(e<1e-12);}
 metric("permutation_flux_max",0,flux_max);
 for(auto tool:{h::Tool::Deposit,h::Tool::Press,h::Tool::Compact,h::Tool::Compress,h::Tool::Scrape,h::Tool::Push,h::Tool::Smear,h::Tool::Melt,h::Tool::Grab}){
  auto target=h::target(original,tool,{},Vec3{1,0,0},1,.05);
  auto ua=h::optimize(original,target);
  auto ub=h::optimize(reordered,h::target(reordered,tool,{},Vec3{1,0,0},1,.05));
  double maxerr=0,err2=0,area=0,requested=0,applied=0;std::size_t changed=0;
  for(std::size_t i=0;i<q.size();++i){double e=h::norm(ua[i]-ub[perm[i]]);check(e<1e-10);maxerr=std::max(maxerr,e);double a=original.area[i];area+=a;double d=h::norm(ua[i]-target[i]);err2+=a*d*d;requested+=a*h::norm(target[i]);applied+=a*h::norm(ua[i]);if(d>1e-12)++changed;}
  int id=static_cast<int>(tool);metric("permutation_displacement_max",id,maxerr);metric("target_error_rms",id,std::sqrt(err2/area));metric("modified_vertex_fraction",id,double(changed)/q.size());if(requested>0)metric("applied_requested_norm_ratio",id,applied/requested);
  std::vector<double> times;for(int k=0;k<100;++k){auto start=std::chrono::steady_clock::now();auto result=h::optimize(original,target);auto end=std::chrono::steady_clock::now();check(result.size()==q.size());times.push_back(std::chrono::duration<double,std::micro>(end-start).count());}std::sort(times.begin(),times.end());metric("latency_us_p50",id,times[49]);metric("latency_us_p95",id,times[94]);metric("latency_us_p99",id,times[98]);
 }
 // D11.2 stress regression: connected sliver, flat/crest/cavity continuation.
 // Explicit synthetic displacement, not a brush calibration or collision test.
 for(int shape=0;shape<3;++shape){
  double z=shape==0?0.0:(shape==1?.2:-.2);
  std::vector<Vec3> sp{{0,0,0},{1,0,0},{0,.001,0},{1,1,z},{2,0,0},{2,1,0}};
  std::vector<h::Tri> st{{0,1,2},{1,3,2},{1,4,3},{4,5,3}};
  auto patch=h::make_patch(sp,st,{0,1,1,1,1,1});
  std::vector<Vec3> request(sp.size(),Vec3{0,0,.1});
  request[0]={};
  double initial=gradient_motion_scale_v31c(sp,st,request);
  check(initial<.01);
  auto global=request;
  for(auto& v:global)v=v*initial;
  limit_local_contact(sp,st,global,true);
  auto local=request;
  limit_local_gradient_v31d10a(sp,st,local);
  auto hybrid=h::optimize(patch,request);
  auto error=[&](const std::vector<Vec3>& moves){
   double e=0;for(std::size_t i=0;i<moves.size();++i){auto d=moves[i]-request[i];e+=patch.area[i]*h::dot(d,d);}return e;
  };
  auto ratio=[&](const std::vector<Vec3>& moves){
   double requested=0,applied=0;for(std::size_t i=0;i<moves.size();++i){requested+=patch.area[i]*h::norm(request[i]);applied+=patch.area[i]*h::norm(moves[i]);}return applied/requested;
  };
  for(const auto* moves:{&global,&local,&hybrid}){
   check(h::norm((*moves)[0])==0);
   check(gradient_motion_scale_v31c(sp,st,*moves)>=1-1e-10);
   for(auto tri:st){ContactTriangle ct{};for(int j=0;j<3;++j){ct.position[j]=sp[tri[j]];ct.motion[j]=(*moves)[tri[j]];}check(contact_triangle_scale(ct)>=1-1e-10);}
  }
  check(error(hybrid)<=error(local)+1e-12);
  check(error(hybrid)<error(global));
  // A merely equal D10A fallback is no longer sufficient on this fixture.
  check(error(hybrid)<.95*error(local));
  check(hybrid[5].z>=local[5].z-1e-12);
  // Reorder both vertices and face list, preserving winding and support.
  auto rp=sp;auto rt=st;auto rr=request;auto rw=patch.weight;
  std::reverse(rp.begin(),rp.end());std::reverse(rr.begin(),rr.end());std::reverse(rw.begin(),rw.end());
  for(auto& tri:rt)for(auto& i:tri)i=sp.size()-1-i;
  std::reverse(rt.begin(),rt.end());
  auto reversed=h::optimize(h::make_patch(rp,rt,rw),rr);
  double permutation_error=0;
  for(std::size_t i=0;i<sp.size();++i)permutation_error=std::max(permutation_error,h::norm(hybrid[i]-reversed[sp.size()-1-i]));
  metric("stress_permutation_max",shape,permutation_error);
  check(permutation_error<1e-7);

  std::size_t changed=0;for(std::size_t i=0;i<request.size();++i)if(h::norm(hybrid[i]-request[i])>1e-12)++changed;
  check(changed>0);
  metric("stress_initial_gradient_scale",shape,initial);
  metric("stress_global_error",shape,error(global));metric("stress_d10a_error",shape,error(local));metric("stress_d11_error",shape,error(hybrid));
  metric("stress_global_ratio",shape,ratio(global));metric("stress_d10a_ratio",shape,ratio(local));metric("stress_d11_ratio",shape,ratio(hybrid));
  metric("stress_global_far_z",shape,global[5].z);metric("stress_d10a_far_z",shape,local[5].z);metric("stress_d11_far_z",shape,hybrid[5].z);
  metric("stress_modified_vertex_fraction",shape,double(changed)/request.size());
 }
 // D11.3: continuous swept contact, nearby disconnected sheet and barriers.
 {
  std::vector<Vec3> cp{{0,0,0},{1,0,0},{0,.25,0},{1,.25,0},{0,0,.01},{1,0,.01},{0,.25,.01}};
  std::vector<h::Tri> ct{{0,1,2},{1,3,2},{4,5,6}};
  auto patch=h::make_patch(cp,ct,std::vector<double>(cp.size(),1));
  std::vector<double> visible(cp.size(),1),permit(cp.size(),1);
  auto c=h::swept_contact(patch,{0,1},cp[0],cp[1],.5,2,visible,permit);
  check(c.weight[0]==1&&c.weight[1]==1);
  check(c.weight[2]>0&&c.weight[2]<1&&c.weight[3]>0);
  check(c.weight[4]==0&&c.weight[5]==0&&c.weight[6]==0);
  auto reverse=h::swept_contact(patch,{1,0},cp[1],cp[0],.5,2,visible,permit);
  for(std::size_t i=0;i<cp.size();++i)check(std::abs(c.weight[i]-reverse.weight[i])<1e-12);
  visible[1]=0;permit[2]=0;
  auto blocked=h::swept_contact(patch,{0,1},cp[0],cp[1],.5,2,visible,permit);
  check(blocked.weight[1]==0&&blocked.weight[2]==0&&blocked.weight[3]==0);
  visible[1]=1;permit[2]=.25;
  auto masked=h::swept_contact(patch,{0,1},cp[0],cp[1],.5,2,visible,permit);
  check(std::abs(masked.weight[2]-.25*c.weight[2])<1e-12);
  // Capsule boundary approaches zero continuously.
  auto narrow=h::swept_contact(patch,{0,1},cp[0],cp[1],.250001,2,visible,permit);
  auto edge=h::swept_contact(patch,{0,1},cp[0],cp[1],.25,2,visible,permit);
  check(narrow.weight[3]>0&&narrow.weight[3]<1e-8&&edge.weight[3]==0);
  metric("swept_connected_weight",0,c.weight[3]);
  metric("swept_disconnected_leak",0,c.weight[4]+c.weight[5]+c.weight[6]);
  metric("swept_masked_barrier_leak",0,blocked.weight[3]);
  metric("swept_boundary_weight",0,narrow.weight[3]);
  patch.weight=blocked.weight;
  auto moves=h::optimize(patch,h::target(patch,h::Tool::Deposit,{},Vec3{0,0,1},.5,.05));
  for(std::size_t i=1;i<cp.size();++i)check(h::norm(moves[i])==0);
  bool bad=false;try{h::swept_contact(patch,{cp.size()},cp[0],cp[1],.5,2,visible,permit);}
  catch(const std::invalid_argument&){bad=true;}check(bad);
 }
 // D11.4: fixed physical unit square, 1x/2x/4x/8x linear resolution.
 // Density means samples per axis here (triangle count scales quadratically).
 auto grid=[](int cells,bool pin){
  std::vector<Vec3> p;std::vector<h::Tri> t;std::vector<double> w;
  for(int y=0;y<=cells;++y)for(int x=0;x<=cells;++x){
   p.push_back({double(x)/cells,double(y)/cells,1});
   w.push_back(pin&&(x==0||y==0||x==cells||y==cells)?0.:1.);
  }
  for(int y=0;y<cells;++y)for(int x=0;x<cells;++x){
   std::size_t a=std::size_t(y)*(cells+1)+x,b=a+1,c=a+cells+1,d=c+1;
   t.push_back({a,b,c});t.push_back({b,d,c});
  }
  return h::make_patch(p,t,w);
 };
 for(int density:{1,2,4,8}){
  auto patch=grid(4*density,false);
  std::vector<double> pressure;for(auto v:patch.p)pressure.push_back(.5+.5*std::cos(3.141592653589793*v.x));
  auto start=std::chrono::steady_clock::now();
  auto stencil=h::transport_stencil(patch,1);
  auto end=std::chrono::steady_clock::now();
  auto f=h::transport_flux(stencil,pressure);
  auto old=h::flux(patch,pressure,1);
  double net=0,absolute=0,old_abs=0,moment=0;
  for(std::size_t i=0;i<f.size();++i){
   check(std::isfinite(f[i])&&std::abs(f[i])<=.02+1e-12);
   net+=patch.area[i]*f[i];absolute+=patch.area[i]*std::abs(f[i]);
   old_abs+=patch.area[i]*std::abs(old[i]);moment+=patch.area[i]*patch.p[i].x*f[i];
  }
  check(std::abs(net)<1e-12&&absolute>0&&moment>0);
  auto reverse=patch.p;auto rt=patch.tris;auto rw=patch.weight;auto rp=pressure;
  std::reverse(reverse.begin(),reverse.end());std::reverse(rw.begin(),rw.end());std::reverse(rp.begin(),rp.end());
  for(auto& tri:rt)for(auto& i:tri)i=reverse.size()-1-i;
  std::reverse(rt.begin(),rt.end());
  auto back=h::transport_flux(h::transport_stencil(h::make_patch(reverse,rt,rw),1),rp);
  double permutation=0;for(std::size_t i=0;i<f.size();++i)permutation=std::max(permutation,std::abs(f[i]-back[f.size()-1-i]));
  check(permutation<1e-12);
  metric("density_vertices",density,double(f.size()));
  metric("density_pairs",density,double(stencil.pairs.size()));
  metric("density_legacy_absolute_proxy",density,old_abs);
  metric("density_nonlocal_absolute_proxy",density,absolute);
  metric("density_nonlocal_net",density,net);
  metric("density_nonlocal_x_moment",density,moment);
  metric("density_permutation_max",density,permutation);
  metric("density_stencil_build_us",density,std::chrono::duration<double,std::micro>(end-start).count());
 }
 // Rebuild on evolving geometry, apply the accepted displacement only.
 // Closed unit prism with fixed bottom and sides: difference of signed
 // tetrahedron sums on the moving top equals actual enclosed volume change.
 {
  auto patch=grid(4,true);
  const auto original=patch.p;
  auto volume_top=[&](const std::vector<Vec3>& pos){
   double v=0;for(auto tri:patch.tris)v+=h::dot(pos[tri[0]],h::cross(pos[tri[1]],pos[tri[2]]))/6;
   return v;
  };
  const double initial_volume_top=volume_top(original);
  double cumulative_proxy=0,cumulative_abs=0;
  auto start=std::chrono::steady_clock::now();
  for(int step=1;step<=1000;++step){
   std::vector<double> pressure;for(auto v:original)pressure.push_back(1-v.x);
   auto f=h::transport_flux(h::transport_stencil(patch,1),pressure);
   std::vector<Vec3> desired(f.size());for(std::size_t i=0;i<f.size();++i)desired[i]=patch.normal[i]*(.02*f[i]);
   auto accepted=h::optimize(patch,desired,true);
   check(gradient_motion_scale_v31c(patch.p,patch.tris,accepted)>=1-1e-10);
   double net=0,absolute=0;
   for(std::size_t i=0;i<f.size();++i){
    if(patch.weight[i]==0)check(h::norm(accepted[i])==0);
    double v=patch.area[i]*h::dot(patch.normal[i],accepted[i]);
    net+=v;absolute+=std::abs(v);
   }
   check(std::abs(net)<=1e-12+1e-9*absolute);
   cumulative_proxy+=net;cumulative_abs+=absolute;
   auto pos=patch.p;for(std::size_t i=0;i<pos.size();++i)pos[i]+=accepted[i];
   patch=h::make_patch(pos,patch.tris,patch.weight);
   if(step==100||step==1000){
    double drift=volume_top(patch.p)-initial_volume_top;
    check(std::isfinite(drift)&&cumulative_abs>0);
    metric("repeat_cumulative_signed_proxy",step,cumulative_proxy);
    metric("repeat_cumulative_absolute_proxy",step,cumulative_abs);
    metric("repeat_closed_volume_drift",step,drift);
    metric("repeat_volume_drift_relative",step,std::abs(drift)/cumulative_abs);
    metric("repeat_elapsed_ms",step,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
   }
  }
 }
 std::cout<<"hybrid foundation tests passed\n";
}
