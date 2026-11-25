/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "multichart_contact_v31d7.hh"
#include <cstdlib>
#include <iostream>

using namespace sculpt_physics;
static void require(bool ok,const char *msg)
{
  if(!ok){std::cerr<<msg<<'\n';std::exit(EXIT_FAILURE);}
}
static double length(Vec3 v){return std::hypot(v.x,v.y,v.z);}

int main()
{
  // A right-angle continuous fold, not two disconnected surfaces.
  // Horizontal plane has +Z normals, vertical wall has +X normals.
  // Both face the oblique camera, but the original tool chart is +Z.
  constexpr int rows=11,cols=9,corner=5;
  std::vector<ClaySurfaceVertex> v;
  std::vector<ClaySurfaceTriangle> t;
  for(int y=0;y<cols;++y)for(int x=0;x<rows;++x){
    const double px=x<=corner?.10*(x-corner):0.;
    const double pz=x<=corner?0.: -.10*(x-corner);
    const double py=.10*(y-(cols-1)/2);
    v.push_back({{px,py,pz},1});
  }
  for(int y=0;y<cols-1;++y)for(int x=0;x<rows-1;++x){
    const auto a=std::size_t(y*rows+x),b=a+1,c=a+rows,d=c+1;
    t.push_back({a,b,d});t.push_back({a,d,c});
  }
  // A detached close parallel sheet must not be recovered.
  const auto other=v.size();
  v.push_back({{.04,-.08,-.12},1});
  v.push_back({{.04,-.08,-.22},1});
  v.push_back({{.04,.08,-.12},1});
  t.push_back({other,other+1,other+2});
  const auto pinned=std::size_t(2*rows+8);
  v[pinned].influence=0;
  ClaySurfaceDab dab;
  dab.action=ClaySurfaceAction::Scrape;
  dab.axis={0,0,1};dab.radius=.9;dab.dose=.08;dab.intensity=1;
  dab.extended_material=true;dab.density_independent=true;dab.smooth_proxy=true;
  dab.shape_transport=true;dab.tool_v25=true;dab.tool_v26=true;
  dab.stable_v28=true;dab.refine_v28b=true;dab.live_surface_v30=true;
  dab.contact_v30b=true;dab.fast_v26b=true;dab.adaptive_v31=true;
  dab.average_surface_v31d=.65;dab.continuous_contact_v31d2=true;
  dab.gradient_v31c=true;dab.motion_proxy_v31d3=.65;
  dab.has_tool_plane=true;dab.tool_plane_height=-dab.tool_depth*dab.radius;
  const auto baseline=solve_material_transport_v21(v,t,dab,4,false).surface;
  ClaySurfaceResult out;MultiChartContactV31D7 stats;
  require(solve_multichart_contact_v31d7(v,t,dab,{1,0,1},4,out,stats),
          "fold needs and accepts local second chart");
  require(stats.charts==2 && stats.recovered>0 && stats.steep>=2,
          "two active charts and recovered steep wall");
  const auto wall=std::size_t(4*rows+8);
  require(length(baseline.displacement[wall])<1e-15,"single-chart wall rejected");
  require(out.displacement[wall].x< -1e-15,"local chart sculpts vertical wall");
  require(length(out.displacement[pinned])==0,"masked vertex stays fixed");
  for(std::size_t i=other;i<v.size();++i)
    require(length(out.displacement[i])==0,"near detached sheet stays fixed");
  std::vector<Vec3> p;
  for(const auto &a:v)p.push_back(a.position);
  auto accepted=out.displacement;
  const auto guard=limit_local_contact(p,t,accepted,true);
  require(guard.final_scale>.999999,"combined motion passed real triangle guard");
  require(std::isfinite(stats.signed_volume) && stats.final_scale>0,
          "finite first-order budget");

  // Flat geometry must take the exact original fast path.
  auto flat=v;flat.resize(other);
  auto flat_faces=t;flat_faces.pop_back();
  for(int y=0;y<cols;++y)for(int x=0;x<rows;++x)
    flat[std::size_t(y*rows+x)].position={.10*(x-corner),.10*(y-(cols-1)/2),0};
  MultiChartContactV31D7 flat_stats;ClaySurfaceResult unused;
  require(!solve_multichart_contact_v31d7(flat,flat_faces,dab,{0,0,1},4,unused,flat_stats),
          "flat surface uses original solver");
  // A complete zero-factor seam separates the steep wall.
  auto barrier=v;
  for(int y=0;y<cols;++y)barrier[std::size_t(y*rows+corner+1)].influence=0;
  MultiChartContactV31D7 barrier_stats;
  require(!solve_multichart_contact_v31d7(barrier,t,dab,{1,0,1},4,unused,barrier_stats),
          "no recovery through fully masked seam");
  dab.dose=0;
  MultiChartContactV31D7 zero_stats;
  require(!solve_multichart_contact_v31d7(v,t,dab,{1,0,1},4,unused,zero_stats),
          "zero dose no-op");
  std::cout<<"D7 multichart contact tests passed\n";
  return EXIT_SUCCESS;
}
