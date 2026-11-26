#include "artistic_relief_v31d1110.hh"
#include "artistic_swept_v31d1111.hh"
#include "artistic_csr_smooth_v31d1112.hh"
#include <cstdlib>
#include <limits>
using namespace sculpt_physics::artistic_v31d1110;
static void check(bool b){if(!b)std::abort();}
int main(){
 auto soft=evaluate(Material::SoftClay,1,0,1);
 auto firm=evaluate(Material::FirmClay,1,0,1);
 auto mud=evaluate(Material::Mud,1,0,1);
 auto wax=evaluate(Material::Wax,1,0,1);
 auto foam=evaluate(Material::Foam,1,0,1);
 auto flesh=evaluate(Material::Flesh,1,0,1);
 check(soft.deposit==firm.deposit && soft.transport>firm.transport);
 check(mud.transport>wax.transport);
 check(foam.elastic_sink>flesh.elastic_sink && flesh.elastic_sink>wax.elastic_sink);
 check(evaluate(Material::Putty,1,0,1).drag>soft.drag);
 check(evaluate(Material::Wood,1,0,1).drag>evaluate(Material::Wood,1,0,0).drag);
 check(evaluate(Material::FirmClay,1,1,1).plastic_sink<firm.plastic_sink);
 for(int i=0;i<9;++i){
  auto z=evaluate(static_cast<Material>(i),0,0,1);
  check(z.deposit==0 && z.transport==0 && z.drag==0 && z.sink==0);
 }
 Intent i{.2,.1,.5,0,1};
 auto build=target(Action::Build,Material::FirmClay,i);
 check(build.normal_deposit==i.native_deposit);
 auto push=target(Action::Push,Material::Mud,i);
 auto smear=target(Action::Smear,Material::Mud,i);
 check(push.transport>smear.transport);
 auto press=target(Action::Press,Material::Foam,i);
 auto compact=target(Action::Compact,Material::Foam,i);
 check(compact.axial_sink>press.axial_sink && compact.transport<press.transport);
 check(target(Action::Gather,Material::Putty,i).radial_gather>0);
 check(target(Action::Melt,Material::Mud,i).relaxation>0);
 for(int n: {1,2,8,64,256}){
  double moving=0,time=0;
  for(int j=0;j<n;++j){
   auto d=integrate({1./n,2./n,.2+.6*j/n,.2+.6*(j+1)/n,.1});
   moving+=d.moving;time+=d.stationary;
  }
  check(std::abs(moving-.5)<1e-12 && std::abs(time-1.)<1e-12);
 }
 auto stationary=integrate({0,1,.5,.5,.1});
 check(stationary.moving==0 && stationary.stationary==.5);
 auto patch=h::make_patch({{0,0,0},{1,0,0},{0,1,0}},{{0,1,2}},{1,1,0});
 for(int a=0;a<8;++a)for(int m=0;m<9;++m){
  auto u=geometry_target(patch,static_cast<Action>(a),static_cast<Material>(m),
    {.01,.01,0},{0,0,0},{1,0,1},{.3,.3,0},.01,1);
  check(u.size()==3 && h::norm(u[2])==0);
  for(auto v:u)check(std::isfinite(h::norm(v)));
 }
 auto u=geometry_target(patch,Action::Build,Material::FirmClay,
    {.01,.01,0},{0,0,0},{1,0,1},{.3,.3,0},.01,1);
 check(u[0].z>0);
 auto flux=pair_transport(patch,{1,0,0},4,.1);
 check(flux[0]<0 && flux[1]>0 && flux[2]==0);
 double balance=0;
 for(std::size_t k=0;k<flux.size();++k)balance+=flux[k]*patch.area[k];
 check(std::abs(balance)<1e-12);
 auto uniform=pair_transport(patch,{1,1,0},4,.1);
 for(auto x:uniform)check(x==0);
 // Dense planar grid: exercise cached transport across a connected patch,
 // partial falloff and a fully pinned boundary. Compare to exact stencil.
 std::vector<sculpt_physics::Vec3> grid_p;
 std::vector<h::Tri> grid_t;
 std::vector<double> grid_w,grid_drive;
 constexpr std::size_t side=24;
 for(std::size_t y=0;y<side;++y)for(std::size_t x=0;x<side;++x){
  grid_p.push_back({double(x)/double(side-1),double(y)/double(side-1),0});
  grid_w.push_back(x==0?0.:.25+.75*double(y)/double(side-1));
  grid_drive.push_back(double(x)/double(side-1));
  if(x+1<side&&y+1<side){
   auto k=y*side+x;
   grid_t.push_back({k,k+1,k+side});
   grid_t.push_back({k+1,k+side+1,k+side});
  }
 }
 auto grid=h::make_patch(grid_p,grid_t,grid_w);
 h::GeodesicCache cache;
 auto expected=pair_transport(grid,grid_drive,.25,.1);
 auto cached=pair_transport(grid,grid_drive,.25,.1,&cache);
 check(!cache.pairs.empty());
 auto replay=pair_transport(grid,grid_drive,.25,.1,&cache);
 double net=0.;
 for(std::size_t k=0;k<cached.size();++k){
  check(std::abs(cached[k]-expected[k])<1e-10);
  check(std::abs(replay[k]-cached[k])<1e-10);
  if(grid_w[k]==0)check(cached[k]==0.);
  net+=cached[k]*grid.area[k];
 }
 check(std::abs(net)<1e-12);
 h::GeodesicCache empty_cache;
 auto uniform_grid=pair_transport(grid,std::vector<double>(grid_p.size(),.5),.25,.1,&empty_cache);
 check(empty_cache.pairs.empty());
 for(auto x:uniform_grid)check(x==0.);
 // Brush-scale relief tests on a triangulated disk footprint.
 std::vector<sculpt_physics::Vec3> relief_p;
 std::vector<h::Tri> relief_t;
 std::vector<double> relief_w;
 constexpr std::size_t dim=33;
 for(std::size_t y=0;y<dim;++y)for(std::size_t x=0;x<dim;++x){
  const double a=2.*double(x)/double(dim-1)-1.;
  const double b=2.*double(y)/double(dim-1)-1.;
  relief_p.push_back({a,b,0});
  relief_w.push_back(std::max(0.,1.-a*a-b*b));
  if(x+1<dim&&y+1<dim){
   const auto k=y*dim+x;
   relief_t.push_back({k,k+1,k+dim});
   relief_t.push_back({k+1,k+dim+1,k+dim});
  }
 }
 auto flat=h::make_patch(relief_p,relief_t,relief_w);
 auto push_relief=relief_target(flat,Action::Push,Material::Mud,
    {0,0,0},{1,0,0},{0,0,1},1.,.04,1.);
 auto gather_relief=relief_target(flat,Action::Gather,Material::Mud,
    {0,0,0},{1,0,0},{0,0,1},1.,.04,1.);
 double push_moment=0.,gather_center=0.,push_net=0.,gather_net=0.;
 for(std::size_t k=0;k<relief_p.size();++k){
  const double area=flat.area[k];
  push_moment+=relief_p[k].x*push_relief[k].z*area;
  gather_center+=gather_relief[k].z*area*(1.-h::norm(relief_p[k]));
  push_net+=push_relief[k].z*area;gather_net+=gather_relief[k].z*area;
  if(relief_w[k]==0)check(h::norm(push_relief[k])==0&&h::norm(gather_relief[k])==0);
 }
 check(push_moment>0.&&gather_center>0.);
 check(std::abs(push_net)<1e-10&&std::abs(gather_net)<1e-10);
 for(auto& q:relief_p)q.z=.08*std::exp(-20.*(q.x*q.x+q.y*q.y));
 auto bump=h::make_patch(relief_p,relief_t,relief_w);
 auto melt=relief_target(bump,Action::Melt,Material::Mud,
    {0,0,0},{1,0,0},{0,0,1},1.,.04,1.);
 const auto mid=dim*(dim/2)+dim/2;
 check(h::dot(melt[mid],bump.normal[mid])<0.);
 auto smear_relief=relief_target(bump,Action::Smear,Material::Putty,
    {0,0,0},{1,0,0},{0,0,1},1.,.04,1.);
 double smear_peak=0.;
 for(std::size_t k=0;k<smear_relief.size();++k){
  smear_peak=std::max(smear_peak,h::norm(smear_relief[k]));
  if(relief_w[k]==0)check(h::norm(smear_relief[k])==0.);
 }
 check(smear_peak>0.);
 std::vector<sculpt_physics::Vec3> deposit_relief(flat.p.size());
 for(std::size_t k=0;k<flat.p.size();++k)
  deposit_relief[k]={0,0,.01*std::pow(relief_w[k],8)};
 auto soft_spread=spread_deposit(flat,deposit_relief,Material::SoftClay,1.,.04,1.);
 auto firm_spread=spread_deposit(flat,deposit_relief,Material::FirmClay,1.,.04,1.);
 check(soft_spread[mid].z<firm_spread[mid].z);
 auto transported=geometry_target(patch,Action::Press,Material::Mud,
    {0,0,0},{0,.8,0},{0,0,1},{.3,.3,0},.01,1,4);
 check(h::norm(transported[2])==0);
 for(auto v:transported)check(std::isfinite(h::norm(v)));
 auto sweep=swept_exposure(patch,{0,1},{0,0,0},{1,0,0},.2,2,1,1,1,
    {1,1,1},{1,1,0});
 check(sweep.moving[0]>0 && sweep.moving[1]>0 && sweep.moving[2]==0);
 auto dwell=swept_exposure(patch,{0},{0,0,0},{0,0,0},.2,2,2,.5,.5,
    {1,1,1},{1,1,0});
 check(dwell.moving[0]==0 && std::abs(dwell.time[0]-1)<1e-12);
 auto blocked=swept_exposure(patch,{0,1},{0,0,0},{1,0,0},.2,2,1,1,1,
    {1,0,1},{1,1,0});
 check(blocked.moving[1]==0);
 auto plane=capture_plane({0,0,-.1},{0,0,1});
 auto scrape=scrape_plane(patch,plane,Material::Mud,{.1,.1,.1},1,1);
 check(scrape[0].z<0 && h::norm(scrape[2])==0);
 auto above=capture_plane({0,0,.1},{0,0,1});
 auto no_cut=scrape_plane(patch,above,Material::Mud,{.1,.1,.1},1,1);
 for(auto v:no_cut)check(h::norm(v)==0);
 check(sculptgl_falloff(0)==1 && sculptgl_falloff(1)==0);
 check(std::abs(sculptgl_falloff(.5)-.6875)<1e-12);
 for(int a=0;a<6;++a)for(int m=0;m<9;++m){
  auto v=surface_target(patch,static_cast<SurfaceAction>(a),static_cast<Material>(m),
    capture_plane({0,0,0},{0,0,1}),{.3,.3,0},{1,0,0},{.1,.1,.1},1,1);
  check(v.size()==3 && h::norm(v[2])==0);
  for(auto x:v)check(std::isfinite(h::norm(x)));
 }
 auto clay=surface_target(patch,SurfaceAction::Clay,Material::FirmClay,
    capture_plane({0,0,0},{0,0,1}),{0,0,0},{1,0,0},{.1,.1,0},1,1);
 check(clay[0].z>0 && clay[0].z<=.1);
 auto work=pack_workset(patch,{.1,.1,0});
 check(work.offsets.size()==4 && work.offsets.back()==6);
 auto csr=csr_smooth(work,Material::Mud,1,1,true);
 auto ref=surface_target(patch,SurfaceAction::SmoothTangent,Material::Mud,
    capture_plane({0,0,0},{0,0,1}),{0,0,0},{1,0,0},{.1,.1,0},1,1);
 auto safe=h::optimize(patch,csr);
 for(std::size_t k=0;k<safe.size();++k)check(h::norm(safe[k]-ref[k])<1e-7);
 check(h::norm(csr[2])==0);
 bool rejected=false;
 try{evaluate(Material::Mud,std::numeric_limits<double>::quiet_NaN(),0,1);}
 catch(const std::invalid_argument&){rejected=true;}
 check(rejected);
}
