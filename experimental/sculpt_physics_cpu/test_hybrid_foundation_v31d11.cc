#include "hybrid_foundation_v31d11.hh"
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <locale>
using namespace sculpt_physics;
namespace h=hybrid_v31d11;
#define check(x) do { if(!(x)) { std::cerr << "failure at line " << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while(false)
// Frozen D11.4 builder for same-run numerical and timing A/B.
inline h::TransportStencil reference_stencil(const h::Patch& s,double radius){
 if(!std::isfinite(radius)||radius<=0)throw std::invalid_argument("transport radius");
 h::TransportStencil out;out.size=s.p.size();out.area=s.area;
 const double reach=radius*.5;
 if(!(reach>0))throw std::invalid_argument("transport reach");
 const std::size_t n=out.size;
 struct Raw {std::size_t i,j;double kernel;};
 std::vector<Raw> raw;
 std::vector<double> support(n),dist(n,std::numeric_limits<double>::infinity());
 std::vector<std::size_t> touched;
 using Q=std::pair<double,std::size_t>;
 auto allowed=[&](std::size_t i){return s.weight[i]>0&&s.area[i]>0;};
 for(std::size_t i=0;i<n;++i){
  if(!allowed(i))continue;
  support[i]+=s.area[i]; // self quadrature, no self transfer
  std::priority_queue<Q,std::vector<Q>,std::greater<Q>> q;
  dist[i]=0;touched.push_back(i);q.push({0,i});
  while(!q.empty()){
   auto [d,j]=q.top();q.pop();if(d!=dist[j])continue;
   if(j>i){
    double x=1-d/reach;
    double k=x*x*(3-2*x);
    raw.push_back({i,j,k});
    support[i]+=s.area[j]*k;
    support[j]+=s.area[i]*k;
   }
   for(auto k:s.adj[j])if(allowed(k)){
    double next=d+h::norm(s.p[k]-s.p[j]);
    if(next<reach&&next<dist[k]){
     if(!std::isfinite(dist[k]))touched.push_back(k);
     dist[k]=next;q.push({next,k});
    }
   }
  }
  for(auto j:touched)dist[j]=std::numeric_limits<double>::infinity();
  touched.clear();
 }
 for(auto e:raw){
  const auto i=e.i,j=e.j;
  double c=radius*.02*std::min(s.weight[i],s.weight[j])*s.area[i]*
           (s.area[j]/std::max(support[i],support[j]))*e.kernel;
  if(!std::isfinite(c))throw std::overflow_error("transport coefficient");
  out.pairs.push_back({i,j,c});
 }
 return out;
}

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
  auto ref_start=std::chrono::steady_clock::now();
  auto ref=reference_stencil(patch,1);
  auto ref_end=std::chrono::steady_clock::now();
  auto ref_flux=h::transport_flux(ref,pressure);
  check(ref.pairs.size()==stencil.pairs.size());
  double coefficient_error=0,flux_error=0;
  for(std::size_t i=0;i<ref.pairs.size();++i){
   check(ref.pairs[i].i==stencil.pairs[i].i&&ref.pairs[i].j==stencil.pairs[i].j);
   coefficient_error=std::max(coefficient_error,std::abs(ref.pairs[i].coefficient-stencil.pairs[i].coefficient));
  }
  for(std::size_t i=0;i<f.size();++i)flux_error=std::max(flux_error,std::abs(ref_flux[i]-f[i]));
  check(coefficient_error<1e-14&&flux_error<1e-12);
  metric("transport_reference_build_us",density,std::chrono::duration<double,std::micro>(ref_end-ref_start).count());
  metric("transport_ab_coefficient_max",density,coefficient_error);
  metric("transport_ab_flux_max",density,flux_error);
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
 // D11.5 material profiles and transactional accepted-write state.
 {
  auto patch=grid(4,false);
  h::PlasticState fresh(patch.p.size());
  double prev=std::numeric_limits<double>::infinity();
  for(auto kind:{h::MaterialKind::SoftClay,h::MaterialKind::Clay,h::MaterialKind::Wax,h::MaterialKind::Wood,h::MaterialKind::Stone}){
   auto press=h::material_target(patch,h::Tool::Press,kind,fresh,Vec3{0,0,1},1,.05);
   auto compact=h::material_target(patch,h::Tool::Compact,kind,fresh,Vec3{0,0,1},1,.05);
   auto compress=h::material_target(patch,h::Tool::Compress,kind,fresh,Vec3{1,0,0},1,.05);
   auto push=h::material_target(patch,h::Tool::Push,kind,fresh,Vec3{1,0,0},1,.05);
   auto smear=h::material_target(patch,h::Tool::Smear,kind,fresh,Vec3{1,0,0},1,.05);
   check(press[12].z<0&&-press[12].z<prev);
   check(compact[12].z<press[12].z&&h::norm(compress[12])<1e-12);
   check(push[12].x>smear[12].x&&smear[12].x>0);
   prev=-press[12].z;
   metric("material_press_depth",static_cast<int>(kind),prev);
   metric("material_smear_distance",static_cast<int>(kind),smear[12].x);
  }
  const auto kind=h::MaterialKind::Clay;
  const auto requested=h::material_target(patch,h::Tool::Press,kind,fresh,Vec3{0,0,1},1,.05);
  auto accepted=h::optimize(patch,requested,true);
  auto pos=patch.p,partial=patch.p;
  for(std::size_t i=0;i<pos.size();++i){pos[i]+=accepted[i];partial[i]+=accepted[i]*.25;}
  auto no=h::plastic_after_write(patch,fresh,h::Tool::Press,kind,1,patch.p);
  auto full=h::plastic_after_write(patch,fresh,h::Tool::Press,kind,1,pos);
  auto part=h::plastic_after_write(patch,fresh,h::Tool::Press,kind,1,partial);
  for(std::size_t i=0;i<pos.size();++i){
   check(fresh[i].compaction==0&&no[i].compaction==0&&no[i].deformation==0);
   check(full[i].compaction>part[i].compaction&&part[i].compaction>0);
   check(full[i].deformation<1e-12); // uniform translation
  }
  metric("plastic_full_compaction",0,full[12].compaction);
  metric("plastic_partial_compaction",0,part[12].compaction);
  auto hardened=h::material_target(patch,h::Tool::Press,kind,full,Vec3{0,0,1},1,.05);
  check(h::norm(hardened[12])<h::norm(requested[12]));
  metric("plastic_hardened_press_ratio",0,h::norm(hardened[12])/h::norm(requested[12]));
  // Carry state across many independent accepted dabs and geometry rebuilds.
  auto state=fresh;
  for(int step=0;step<100;++step){
   auto req=h::material_target(patch,h::Tool::Press,kind,state,Vec3{0,0,1},1,.05);
   auto applied=h::optimize(patch,req,true);
   auto next=patch.p;for(std::size_t i=0;i<next.size();++i)next[i]+=applied[i];
   auto updated=h::plastic_after_write(patch,state,h::Tool::Press,kind,1,next);
   check(updated[12].compaction>=state[12].compaction&&updated[12].compaction<=1);
   state=std::move(updated);patch=h::make_patch(next,patch.tris,patch.weight);
  }
  metric("plastic_compaction_100",0,state[12].compaction);
  check(state[12].compaction>full[12].compaction);
  // Pinned and no-write vertices: reject an invalid transaction atomically.
  auto pinned=grid(4,true);
  h::PlasticState initial(pinned.p.size());
  auto write=pinned.p;write[12].x+=.01;
  auto local=h::plastic_after_write(pinned,initial,h::Tool::Smear,kind,1,write);
  check(local[12].deformation>0&&local[12].compaction==0);
  for(std::size_t i=0;i<write.size();++i)if(i!=12)check(local[i].deformation==0&&local[i].compaction==0);
  write[0].x+=.01;
  bool rejected=false;
  try{h::plastic_after_write(pinned,initial,h::Tool::Smear,kind,1,write);}
  catch(const std::invalid_argument&){rejected=true;}
  check(rejected&&initial[12].deformation==0);
  // Reflection of a planar push, no sign preference.
  auto right=h::material_target(pinned,h::Tool::Push,kind,initial,Vec3{1,0,0},1,.05);
  auto left=h::material_target(pinned,h::Tool::Push,kind,initial,Vec3{-1,0,0},1,.05);
  for(std::size_t i=0;i<right.size();++i)check(h::norm(right[i]+left[i])<1e-12);
  metric("plastic_masked_write_rejected",0,rejected?1:0);
 }
 // Nonuniform state and masked/irregular geometry must retain reference flux.
 {
  auto patch=grid(4,true);
  auto pos=patch.p;pos[12].z+=.17;pos[7].x+=.03;
  patch=h::make_patch(pos,patch.tris,patch.weight);
  std::vector<double> pressure;for(auto v:pos)pressure.push_back(v.x-v.z);
  auto a=h::transport_flux(reference_stencil(patch,1),pressure);
  auto b=h::transport_flux(h::transport_stencil(patch,1),pressure);
  for(std::size_t i=0;i<a.size();++i){check(std::abs(a[i]-b[i])<1e-12);if(patch.weight[i]==0)check(b[i]==0);}
  auto flat=grid(4,true);h::PlasticState fresh(flat.p.size());
  auto start=std::chrono::steady_clock::now();
  auto req=h::material_target(flat,h::Tool::Press,h::MaterialKind::Clay,fresh,Vec3{0,0,1},1,.05);
  auto finish=std::chrono::steady_clock::now();
  for(std::size_t i=0;i<req.size();++i)check(std::abs(req[i].z+(flat.weight[i]>0?.008125:0))<1e-12);
  metric("uniform_material_target_us",0,std::chrono::duration<double,std::micro>(finish-start).count());
 }
 // D11.5 optional aggregate transport: quality and cost relative to exact.
 for(int density:{1,2,4,8}){
  auto patch=grid(4*density,false);
  std::vector<double> pressure;for(auto v:patch.p)pressure.push_back(.5+.5*std::cos(3.141592653589793*v.x));
  auto start=std::chrono::steady_clock::now();
  auto agg=h::aggregate_transport(patch,1);
  auto built=std::chrono::steady_clock::now();
  auto approx=h::aggregate_flux(agg,pressure);
  auto finished=std::chrono::steady_clock::now();
  auto exact_stencil=h::transport_stencil(patch,1);
  auto exact=h::transport_flux(exact_stencil,pressure);
  auto exact_end=std::chrono::steady_clock::now();
  double net=0,err=0,ref=0,abs_a=0,abs_e=0,mom_a=0,mom_e=0;
  for(std::size_t i=0;i<approx.size();++i){
   double a=patch.area[i],d=approx[i]-exact[i];
   check(std::isfinite(approx[i])&&std::abs(approx[i])<=.02+1e-12);
   net+=a*approx[i];err+=a*d*d;ref+=a*exact[i]*exact[i];
   abs_a+=a*std::abs(approx[i]);abs_e+=a*std::abs(exact[i]);
   mom_a+=a*patch.p[i].x*approx[i];mom_e+=a*patch.p[i].x*exact[i];
  }
  check(std::abs(net)<1e-12&&ref>0&&abs_e>0&&mom_e>0);
  double relative=std::sqrt(err/ref);
  metric("aggregate_groups",density,double(agg.groups.size()));
  metric("aggregate_pairs",density,double(agg.pairs.size()));
  metric("aggregate_exact_pairs",density,double(exact_stencil.pairs.size()));
  metric("aggregate_relative_rms",density,relative);
  metric("aggregate_absolute_ratio",density,abs_a/abs_e);
  metric("aggregate_moment_ratio",density,mom_a/mom_e);
  metric("aggregate_net",density,net);
  metric("aggregate_build_us",density,std::chrono::duration<double,std::micro>(built-start).count());
  metric("aggregate_apply_us",density,std::chrono::duration<double,std::micro>(finished-built).count());
  metric("aggregate_exact_total_us",density,std::chrono::duration<double,std::micro>(exact_end-finished).count());
  // Quality is a separate eligibility gate, not a claim that all cases pass.
  // An experimental candidate can be rejected without weakening exact mode.
  bool eligible=relative<=.35&&std::abs(abs_a/abs_e-1)<=.20&&std::abs(mom_a/mom_e-1)<=.20;
  metric("aggregate_quality_gate",density,eligible?1:0);
  if(density==1)check(relative<1e-10);
  if(density==8)check(agg.groups.size()<patch.p.size()&&agg.pairs.size()<exact_stencil.pairs.size());
  auto rev=patch.p;auto rt=patch.tris;auto rw=patch.weight;auto rp=pressure;
  std::reverse(rev.begin(),rev.end());std::reverse(rw.begin(),rw.end());std::reverse(rp.begin(),rp.end());
  for(auto& tri:rt)for(auto& i:tri)i=rev.size()-1-i;
  std::reverse(rt.begin(),rt.end());
  auto back=h::aggregate_flux(h::aggregate_transport(h::make_patch(rev,rt,rw),1),rp);
  double perm=0;for(std::size_t i=0;i<approx.size();++i)perm=std::max(perm,std::abs(approx[i]-back[approx.size()-1-i]));
  check(perm<1e-10);metric("aggregate_permutation_max",density,perm);
 }
 // Two nearby sheets and a fully masked bridge must not exchange material.
 {
  std::vector<Vec3> p{{0,0,0},{.05,0,0},{0,.05,0},{0,0,.001},{.05,0,.001},{0,.05,.001}};
  auto patch=h::make_patch(p,{{0,1,2},{3,4,5}},{1,.25,1,1,1,1});
  auto a=h::aggregate_transport(patch,1);
  auto f=h::aggregate_flux(a,{1,1,1,0,0,0});
  check(a.groups.size()==2&&a.pairs.empty());
  for(auto x:f)check(std::abs(x)<1e-12);
  auto bridge=grid(8,false);
  std::vector<double> pressure;
  for(std::size_t i=0;i<bridge.p.size();++i){
   if(bridge.p[i].x==.5)bridge.weight[i]=0;
   pressure.push_back(bridge.p[i].x<.5?1:0);
  }
  auto blocked=h::aggregate_flux(h::aggregate_transport(bridge,1),pressure);
  double leak=0;for(double x:blocked)leak=std::max(leak,std::abs(x));
  check(leak<1e-12);metric("aggregate_barrier_leak",0,leak);
  auto varied=h::aggregate_flux(a,{1,0,.5,0,0,0});
  double net=0;for(std::size_t i=0;i<varied.size();++i){
   net+=patch.area[i]*varied[i];check(std::abs(varied[i])<=.02*patch.weight[i]+1e-12);
   if(i>=3)check(std::abs(varied[i])<1e-12);
  }
  check(std::abs(net)<1e-12&&std::abs(varied[0])>0);
  bool bad=false;try{h::aggregate_flux(a,{2,0,0,0,0,0});}catch(const std::invalid_argument&){bad=true;}check(bad);
 }
 // D11.5 certified block study: strict per-coefficient bound, not
 // a guaranteed relative norm bound in the presence of cancellation.
 for(int density:{1,2,4,8}){
  auto patch=grid(4*density,false);
  // Include fractional falloff: no change from min(weight_i,weight_j).
  for(std::size_t i=0;i<patch.p.size();++i)patch.weight[i]=.25+.75*patch.p[i].x;
  auto exact_stencil=h::transport_stencil(patch,1);
  for(int tol_case:{0,1,2}){
   double tolerance=tol_case==0?.05:(tol_case==1?.15:.30);
   auto start=std::chrono::steady_clock::now();
   auto block=h::block_transport(patch,1,.125,tolerance);
   auto built=std::chrono::steady_clock::now();
   check(block.exact.size()+block.represented_pairs==exact_stencil.pairs.size());
   int id=10*density+tol_case;
   metric("block_exact_pairs",id,double(block.exact.size()));
   metric("block_compressed_pairs",id,double(block.represented_pairs));
   metric("block_count",id,double(block.blocks.size()));
   metric("block_build_us",id,std::chrono::duration<double,std::micro>(built-start).count());
   // Smooth pressure and sharp edge: evaluate both against the same stencil.
   for(int shape:{0,1}){
    std::vector<double> pressure;
    for(auto v:patch.p)pressure.push_back(shape==0?.5+.5*std::cos(3.141592653589793*v.x):(v.x<.5?1.:0.));
    auto es=std::chrono::steady_clock::now();
    auto exact=h::transport_flux(exact_stencil,pressure);
    auto ee=std::chrono::steady_clock::now();
    auto approx=h::block_flux(block,pressure);
    auto be=std::chrono::steady_clock::now();
    double net=0,error=0,ref=0,ab=0,eb=0,energy=0,maximum=0;
    for(std::size_t i=0;i<exact.size();++i){
     double a=patch.area[i],d=approx[i]-exact[i];
     check(std::abs(d)<=tolerance*.02*patch.weight[i]+1e-12);
     check(std::abs(approx[i])<=.02*patch.weight[i]+1e-12);
     net+=a*approx[i];energy+=a*pressure[i]*approx[i];
     error+=a*d*d;ref+=a*exact[i]*exact[i];ab+=a*std::abs(approx[i]);eb+=a*std::abs(exact[i]);
     maximum=std::max(maximum,std::abs(d));
    }
    check(std::abs(net)<1e-12&&energy<=1e-12&&ref>0&&eb>0);
    int k=2*id+shape;
    metric("block_relative_rms",k,std::sqrt(error/ref));
    metric("block_absolute_ratio",k,ab/eb);
    metric("block_max_absolute_error",k,maximum);
    metric("block_net",k,net);
    metric("block_exact_apply_us",k,std::chrono::duration<double,std::micro>(ee-es).count());
    metric("block_apply_us",k,std::chrono::duration<double,std::micro>(be-ee).count());
   }
  }
 }
 // Exercise actual nontrivial compression on two tight connected groups
 // with complete far-field support, not merely the all-exact fallback.
 {
  std::vector<Vec3> p{{0,0,0},{.001,0,0},{0,.001,0},{.25,0,0},{.251,0,0},{.25,.001,0}};
  auto s=h::make_patch(p,{{0,1,2},{3,4,5},{1,3,2},{2,3,5}},std::vector<double>(6,1));
  auto b=h::block_transport(s,1,.125,.15);
  check(!b.blocks.empty()&&b.represented_pairs>0);
  std::vector<double> pressure{1,.8,.6,.4,.2,0};
  auto a=h::transport_flux(h::transport_stencil(s,1),pressure),f=h::block_flux(b,pressure);
  double net=0;for(std::size_t i=0;i<f.size();++i){net+=s.area[i]*f[i];check(std::abs(a[i]-f[i])<=.003+1e-12);}
  check(std::abs(net)<1e-12);
 }
 // D11.5 geodesic cache: compare against freshly rebuilt reference for
 // every sample, including shape/pressure changes and candidate crossings.
 for(int density:{1,2,4,8}){
  auto base=grid(4*density,false);
  h::GeodesicCache cache;
  double worst=0,worst_bound=0,worst_net=0,exact_us=0,cache_us=0;
  int reused=0;
  for(int step=0;step<8;++step){
   auto p=base.p;auto w=base.weight;
   for(std::size_t i=0;i<p.size();++i){
    p[i].x+=.0001*step*std::sin(3.141592653589793*p[i].x)*std::cos(3.141592653589793*p[i].y);
    p[i].z+=.0003*step*std::sin(6*p[i].x)*std::sin(4*p[i].y);
    w[i]=.25+.75*p[i].x;
   }
   auto patch=h::make_patch(p,base.tris,w);
   std::vector<double> pressure;for(auto v:p)pressure.push_back(.5+.5*std::cos(3.141592653589793*v.x+.1*step));
   auto start=std::chrono::steady_clock::now();
   auto exact=h::transport_flux(h::transport_stencil(patch,1),pressure);
   auto mid=std::chrono::steady_clock::now();
   auto f=h::cached_geodesic_flux(cache,patch,pressure,1);
   auto end=std::chrono::steady_clock::now();
   exact_us+=std::chrono::duration<double,std::micro>(mid-start).count();
   cache_us+=std::chrono::duration<double,std::micro>(end-mid).count();
   reused+=f.reused;
   double err=0,ref=0,net=0;
   for(std::size_t i=0;i<p.size();++i){
    double d=std::abs(exact[i]-f.flux[i]);
    check(d<=f.error_bound[i]+1e-12&&std::abs(f.flux[i])<=.02*patch.weight[i]+1e-12);
    err+=patch.area[i]*d*d;ref+=patch.area[i]*exact[i]*exact[i];net+=patch.area[i]*f.flux[i];
   }
   check(ref>0&&std::abs(net)<1e-12);
   double rel=std::sqrt(err/ref);
   check(rel<=f.relative_bound+1e-10&&f.relative_bound<=.02+1e-12);
   worst=std::max(worst,rel);worst_bound=std::max(worst_bound,f.relative_bound);worst_net=std::max(worst_net,std::abs(net));
  }
  check(reused>0);
  metric("cache_density_reused",density,reused);
  metric("cache_density_candidates",density,double(cache.pairs.size()));
  metric("cache_density_rms_max",density,worst);
  metric("cache_density_bound_max",density,worst_bound);
  metric("cache_density_net_max",density,worst_net);
  metric("cache_density_exact_us",density,exact_us);
  metric("cache_density_total_us",density,cache_us);
 }
 // Actual accepted writes on a pinned closed prism top. Exact and cached
 // flux evaluated on the same evolving geometry to isolate cache error.
 {
  auto patch=grid(4,true);
  h::GeodesicCache cache;
  auto top_volume=[&](const std::vector<Vec3>& p){
   double v=0;for(auto tri:patch.tris)v+=h::dot(p[tri[0]],h::cross(p[tri[1]],p[tri[2]]))/6;return v;
  };
  double initial=top_volume(patch.p),signed_proxy=0,absolute_proxy=0;
  double exact_us=0,cache_us=0,max_error=0,max_bound=0;
  int reused=0;
  for(int step=1;step<=1000;++step){
   std::vector<double> pressure;for(auto v:patch.p)pressure.push_back(std::clamp(1-v.x,0.0,1.0));
   auto a=std::chrono::steady_clock::now();
   auto exact=h::transport_flux(h::transport_stencil(patch,1),pressure);
   auto b=std::chrono::steady_clock::now();
   auto f=h::cached_geodesic_flux(cache,patch,pressure,1);
   auto c=std::chrono::steady_clock::now();
   exact_us+=std::chrono::duration<double,std::micro>(b-a).count();cache_us+=std::chrono::duration<double,std::micro>(c-b).count();
   reused+=f.reused;
   double err=0,ref=0;
   std::vector<Vec3> desired(f.flux.size());
   for(std::size_t i=0;i<f.flux.size();++i){
    double d=std::abs(f.flux[i]-exact[i]);
    check(d<=f.error_bound[i]+1e-12);
    err+=patch.area[i]*d*d;ref+=patch.area[i]*exact[i]*exact[i];
    desired[i]=patch.normal[i]*(.02*f.flux[i]);
   }
   check(ref>0);
   double relative=std::sqrt(err/ref);
   check(relative<=f.relative_bound+1e-10&&f.relative_bound<=.02+1e-12);
   max_error=std::max(max_error,relative);max_bound=std::max(max_bound,f.relative_bound);
   auto accepted=h::optimize(patch,desired,true);
   check(gradient_motion_scale_v31c(patch.p,patch.tris,accepted)>=1-1e-10);
   auto p=patch.p;double net=0,ab=0;
   for(std::size_t i=0;i<p.size();++i){
    if(patch.weight[i]==0)check(h::norm(accepted[i])==0);
    double v=patch.area[i]*h::dot(patch.normal[i],accepted[i]);net+=v;ab+=std::abs(v);p[i]+=accepted[i];
   }
   check(std::abs(net)<=1e-12+1e-9*ab);signed_proxy+=net;absolute_proxy+=ab;
   patch=h::make_patch(p,patch.tris,patch.weight);
   if(step==100||step==1000){
    double drift=top_volume(patch.p)-initial;
    check(std::isfinite(drift)&&absolute_proxy>0&&reused>0);
    metric("cache_repeat_reused",step,reused);
    metric("cache_repeat_exact_us",step,exact_us);
    metric("cache_repeat_total_us",step,cache_us);
    metric("cache_repeat_relative_error_max",step,max_error);
    metric("cache_repeat_relative_bound_max",step,max_bound);
    metric("cache_repeat_signed_proxy",step,signed_proxy);
    metric("cache_repeat_absolute_proxy",step,absolute_proxy);
    metric("cache_repeat_closed_volume_drift",step,drift);
    metric("cache_repeat_volume_drift_relative",step,std::abs(drift)/absolute_proxy);
   }
  }
 }
 // Invalidation and newly entering support, partial mask updates, zero mask
 // barriers, disconnected sheets, permutation (topology snapshot changes).
 {
  auto p=grid(8,false);h::GeodesicCache cache;
  std::vector<double> pressure;for(auto v:p.p)pressure.push_back(1-v.x);
  auto f=h::cached_geodesic_flux(cache,p,pressure,1);
  check(!f.reused);
  auto same=h::cached_geodesic_flux(cache,p,pressure,1);check(same.reused);
  auto moved=p.p;for(auto& v:moved){v.x*=.99;v.y*=.99;}
  auto scaled=h::make_patch(moved,p.tris,p.weight);
  auto scaled_f=h::cached_geodesic_flux(cache,scaled,pressure,1);
  check(scaled_f.reused);
  auto exact=h::transport_flux(h::transport_stencil(scaled,1),pressure);
  for(std::size_t i=0;i<exact.size();++i)check(std::abs(exact[i]-scaled_f.flux[i])<=scaled_f.error_bound[i]+1e-12);
  scaled.weight[0]=.2;check(h::cached_geodesic_flux(cache,scaled,pressure,1).reused);
  for(std::size_t i=0;i<scaled.p.size();++i)if(p.p[i].x==.5)scaled.weight[i]=0;
  for(std::size_t i=0;i<pressure.size();++i)pressure[i]=p.p[i].x<.5?1:0;
  auto masked=h::cached_geodesic_flux(cache,scaled,pressure,1);check(!masked.reused);
  double leak=0;for(auto x:masked.flux)leak=std::max(leak,std::abs(x));
  check(leak<1e-12);metric("cache_mask_leak",0,leak);
  auto changed=scaled.p;for(auto& v:changed)v.x*=1.1;
  auto distorted=h::make_patch(changed,scaled.tris,scaled.weight);
  check(!h::cached_geodesic_flux(cache,distorted,pressure,1).reused);
  std::reverse(distorted.tris.begin(),distorted.tris.end());
  check(!h::cached_geodesic_flux(cache,distorted,pressure,1).reused);
  auto smaller=h::cached_geodesic_flux(cache,distorted,pressure,.8);
  check(smaller.reused);
  auto smaller_exact=h::transport_flux(h::transport_stencil(distorted,.8),pressure);
  for(std::size_t i=0;i<pressure.size();++i)check(std::abs(smaller.flux[i]-smaller_exact[i])<=smaller.error_bound[i]+1e-12);
  std::vector<Vec3> q{{0,0,0},{.05,0,0},{0,.05,0},{0,0,.001},{.05,0,.001},{0,.05,.001}};
  auto sheets=h::make_patch(q,{{0,1,2},{3,4,5}},std::vector<double>(6,1));
  auto isolated=h::cached_geodesic_flux(cache,sheets,{1,1,1,0,0,0},1);
  check(!isolated.reused);for(auto x:isolated.flux)check(std::abs(x)<1e-12);
 }
 // D11.6 material transport selection: same target on unchanged geometry,
 // cached pressure transport only for compression tools.
 {
  auto patch=grid(8,true);
  for(std::size_t i=0;i<patch.p.size();++i)if(patch.weight[i]>0)patch.weight[i]=.3+.7*patch.p[i].x;
  h::PlasticState plastic(patch.p.size());h::GeodesicCache cache;
  for(auto tool:{h::Tool::Press,h::Tool::Compact,h::Tool::Compress}){
   h::CachedFlux d;
   auto a=h::material_target(patch,tool,h::MaterialKind::Clay,plastic,{0,0,1},1,.05);
   auto b=h::material_target(patch,tool,h::MaterialKind::Clay,plastic,{0,0,1},1,.05,&cache,&d);
   for(std::size_t i=0;i<a.size();++i)check(h::norm(a[i]-b[i])<1e-12);
   check(!d.flux.empty()&&d.relative_bound<=.02);
   if(tool!=h::Tool::Press)check(d.reused);
  }
  h::CachedFlux d;
  auto a=h::material_target(patch,h::Tool::Push,h::MaterialKind::Clay,plastic,{1,0,0},1,.05);
  auto b=h::material_target(patch,h::Tool::Push,h::MaterialKind::Clay,plastic,{1,0,0},1,.05,&cache,&d);
  for(std::size_t i=0;i<a.size();++i)check(h::norm(a[i]-b[i])==0);
  check(d.flux.empty());
 }
 // D11.7 narrow ridge with vertical sidewalls, plus a nearby disconnected
 // reverse-wound sheet. Compare old local normal law and feature law.
 {
  std::vector<Vec3> p;std::vector<h::Tri> tris;
  for(auto xz:std::vector<std::pair<double,double>>{{-.2,0},{-.02,0},{-.02,.1},{-.02,.2},{.02,.2},{.02,.1},{.02,0},{.2,0}}){
   p.push_back({xz.first,-.025,xz.second});p.push_back({xz.first,.025,xz.second});
  }
  for(std::size_t i=0;i+2<p.size();i+=2){
   tris.push_back({i,i+2,i+1});tris.push_back({i+2,i+3,i+1});
  }
  auto n=p.size();p.push_back({-.02,-.025,.199});p.push_back({.02,-.025,.199});p.push_back({-.02,.025,.199});
  tris.push_back({n,n+2,n+1});
  auto patch=h::make_patch(p,tris,std::vector<double>(p.size(),1.));
  auto c=h::feature_contact(patch,{0,0,.2},{0,0,1},{0,0,.2},.5);
  check(c.seed<n);
  for(std::size_t i=n;i<p.size();++i)check(c.weight[i]==0);
  check(c.weight[4]>0&&c.weight[10]>0);
  metric("feature_vertical_weight",0,c.weight[4]);
  auto full=patch;for(std::size_t i=n;i<p.size();++i)full.weight[i]=0;
  h::PlasticState plastic(p.size());
  auto old=h::material_target(full,h::Tool::Compress,h::MaterialKind::Clay,plastic,{0,0,1},.5,.05);
  auto target=h::material_target(full,h::Tool::Compress,h::MaterialKind::Clay,plastic,{0,0,1},.5,.05,nullptr,nullptr,true);
  check(h::norm(old[4])<1e-12&&h::norm(target[4])>1e-5);
  metric("feature_vertical_motion",0,h::norm(target[4]));
  patch.weight=c.weight;
  auto desired=h::material_target(patch,h::Tool::Compress,h::MaterialKind::Clay,plastic,{0,0,1},.5,.08,nullptr,nullptr,true);
  bool recovery=false;auto motion=h::optimize_feature(patch,desired,&recovery);
  check(gradient_motion_scale_v31c(patch.p,patch.tris,motion)>=1-1e-10);
  for(std::size_t i=0;i<p.size();++i)if(patch.weight[i]==0)check(h::norm(motion[i])==0);
  metric("feature_vertical_accepted",0,h::norm(motion[4]));
  auto blocked=full;blocked.weight[6]=blocked.weight[7]=0;
  auto masked=h::feature_contact(blocked,{.02,0,.2},{0,0,1},{.02,0,.2},.5);
  check(masked.weight[6]==0&&masked.weight[7]==0&&masked.weight[4]==0);
  auto missing=h::feature_contact(full,{5,0,.2},{0,0,1},{5,0,.2},.5);
  for(auto x:missing.weight)check(x==0);
 }
  // D11.9 field composition: write permission, geometric contact and host
  // falloff remain independent. Their product preserves the D11.8 envelope.
  {
   auto f=h::compose_brush_field({1,1,0},{.8,.8,.8},{1,.25,1});
   check(std::abs(f.weight[0]-.8)<1e-12);
   check(std::abs(f.weight[1]-.2)<1e-12);
   check(f.weight[2]==0);
   auto patch=grid(8,false);
   auto c=h::feature_contact(patch,{.5,.5,0},{0,0,1},{.5,.5,0},1.5);
   std::vector<double> permit(patch.p.size()),falloff(patch.p.size());
   for(std::size_t i=0;i<patch.p.size();++i){
    permit[i]=patch.weight[i]>0?1.:0.;falloff[i]=patch.weight[i];
   }
   auto composed=h::compose_brush_field(permit,c.contact,falloff);
   for(std::size_t i=0;i<patch.p.size();++i)
    check(std::abs(composed.weight[i]-c.weight[i])<1e-12);
   bool bad=false;try{h::compose_brush_field({1},{1,1},{1});}
   catch(const std::invalid_argument&){bad=true;}check(bad);
  }
 // Changing radius within the stored candidate margin can reuse the
 // same distances; the current radius controls kernel and capacity.
 {
  auto patch=grid(8,false);h::GeodesicCache cache;
  std::vector<double> p;for(auto v:patch.p)p.push_back(1-v.x);
  h::cached_geodesic_flux(cache,patch,p,1.);
  auto f=h::cached_geodesic_flux(cache,patch,p,1.005);
  check(f.reused);
  auto exact=h::transport_flux(h::transport_stencil(patch,1.005),p);
  for(std::size_t i=0;i<p.size();++i)check(std::abs(f.flux[i]-exact[i])<=f.error_bound[i]+1e-12);
  check(!h::cached_geodesic_flux(cache,patch,p,1.1).reused);
  metric("cache_small_radius_reused",0,f.reused?1:0);
 }
 std::cout<<"hybrid foundation tests passed\n";
}
