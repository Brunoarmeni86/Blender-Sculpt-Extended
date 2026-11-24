/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "material_transport_v21.hh"
#include "fluid_pbf_v23.hh"
#include "adaptive_material_topology.hh"
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <numeric>

using namespace sculpt_physics;
struct Profile {
  double absolute=0,net=0,center=0,moment_x=0;
  std::vector<Vec3> motion;
};
static Profile profile(int n,ClaySurfaceAction action,double phase=0)
{
  std::vector<ClaySurfaceVertex> vertices;
  std::vector<ClaySurfaceTriangle> tris;
  for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
    const double px=2.0*x/(n-1)-1+phase,py=2.0*y/(n-1)-1;
    const double rr=(px*px+py*py)/(.8*.8);
    const double w=rr<1?std::pow(1-rr,2):0;
    vertices.push_back({{px,py,.12*std::exp(-22*(px*px+py*py))},w});
  }
  for(int y=0;y<n-1;++y)for(int x=0;x<n-1;++x) {
    const auto a=std::size_t(y*n+x),b=a+1,c=a+n,d=c+1;
    tris.push_back({a,b,d});tris.push_back({a,d,c});
  }
  ClaySurfaceDab dab;
  dab.action=action;dab.radius=.8;dab.dose=.20;dab.intensity=.9;
  dab.mobility=.9;dab.direction={1,0,0};dab.friction=.85;
  dab.viscosity=.2;dab.cohesion=.35;dab.stiffness=.25;
  dab.yield_slope=.05;dab.density_independent=true;dab.smooth_proxy=true;
  const auto step=solve_material_transport_v21(vertices,tris,dab,4);
  assert(step.surface.transport_nodes>0);
  Profile out;
  out.motion=step.surface.displacement;
  for(std::size_t i=0;i<vertices.size();++i) {
    const auto &d=out.motion[i];
    const double a=step.surface.projected_area[i];
    assert(d.x==0 && d.y==0 && std::isfinite(d.z));
    if(vertices[i].influence==0)assert(d.z==0);
    out.absolute+=a*std::abs(d.z);out.net+=a*d.z;
    out.moment_x+=a*d.z*vertices[i].position.x;
  }
  out.center=out.motion[(n/2)*n+n/2].z;
  assert(std::abs(out.net)<1e-9);
  assert(out.absolute>1e-8);
  return out;
}
int main()
{
  for(auto a:{ClaySurfaceAction::Push,ClaySurfaceAction::Gather,
              ClaySurfaceAction::Smear,ClaySurfaceAction::Melt}) {
    const auto coarse=profile(33,a),fine=profile(129,a),shift=profile(129,a,.004);
    std::cout<<"action="<<int(a)<<" l1_33="<<coarse.absolute
             <<" l1_129="<<fine.absolute<<" phase_l1="<<shift.absolute
             <<" center="<<fine.center<<" moment_x="<<fine.moment_x<<std::endl;
    assert(fine.absolute/coarse.absolute>.65 && fine.absolute/coarse.absolute<1.55);
    assert(shift.absolute/fine.absolute>.8 && shift.absolute/fine.absolute<1.2);
    if(a==ClaySurfaceAction::Gather) assert(fine.center>0);
    if(a==ClaySurfaceAction::Melt) assert(fine.center<0);
  }
  const auto flow=profile(65,ClaySurfaceAction::Push);
  const auto smear=profile(65,ClaySurfaceAction::Smear);
  const auto melt=profile(65,ClaySurfaceAction::Melt);
  assert(flow.moment_x>smear.moment_x);
  assert(std::abs(melt.moment_x)<std::abs(flow.moment_x)*.1);
  double difference=0;
  for(std::size_t i=0;i<flow.motion.size();++i)
    difference+=std::abs(flow.motion[i].z-smear.motion[i].z);
  assert(difference>1e-6);

  // Liquid compression resolves by separating samples, not by removing
  // particles. Gravity advances free liquid and the sphere is a collider.
  FluidSettingsV23 liquid;
  liquid.gravity={};
  std::vector<FluidParticleV23> particles{{{-.1,0,0},{}},{{.1,0,0},{}}};
  const auto relaxed=step_fluid_v23(particles,liquid);
  assert(relaxed.mass==2 && relaxed.particles.size()==2);
  assert(relaxed.particles[1].position.x-relaxed.particles[0].position.x>.2);
  assert(std::abs(relaxed.particles[0].position.x+relaxed.particles[1].position.x)<1e-12);
  liquid.gravity={0,0,-9.81};
  const auto falling=step_fluid_v23({{{0,0,1},{}}},liquid);
  assert(falling.particles[0].position.z<1);
  liquid.gravity={};liquid.tool_enabled=true;
  const auto collision=step_fluid_v23({{{.1,0,0},{}}},liquid);
  assert(collision.particles[0].position.x>=liquid.tool_radius-1e-12);

  MaterialPatch patch;
  patch.vertices={{{0,0,0},0},{{1,0,0},0},{{0,1,0},0},{{1,1,0},0}};
  MaterialPatchFace a,b;
  a.vertex={0,1,2};a.uv={UV2{0,0},UV2{1,0},UV2{0,1}};
  b.vertex={2,1,3};b.uv={UV2{2,2},UV2{3,2},UV2{3,3}};
  patch.faces={a,b};
  std::vector<Vec3> rest;
  for(const auto &v:patch.vertices)rest.push_back(v.position);
  assert(!stage_material_tear_v23(patch,rest,{true,false},.2).separated);
  patch.vertices[1].position.x=2;
  auto tear=stage_material_tear_v23(patch,rest,{true,false},.2);
  assert(tear.separated && tear.boundary_edges==1 && tear.patch.vertices.size()==6);
  assert(tear.patch.faces[0].vertex[1]!=tear.patch.faces[1].vertex[1]);
  assert(tear.patch.faces[0].uv[1].u==1 && tear.patch.faces[1].uv[1].u==3);
  assert(patch.vertices.size()==4);
}
