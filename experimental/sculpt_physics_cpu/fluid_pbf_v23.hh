/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* Equal-mass 3D position-based liquid reference, based on the density
 * constraint idea of Macklin and Muller (2013). Compression-only
 * constraints, bounded Jacobi iterations, sphere contact and symmetric
 * velocity diffusion. No native Blender writeback, free-surface extraction,
 * surface tension, continuous collision detection or GPU kernel. */
struct FluidParticleV23 { Vec3 position{},velocity{}; };
struct FluidSettingsV23 {
  double support=1, mass=1, rest_density=2.35, dt=.01;
  double viscosity=.1, relaxation=1e-6;
  int iterations=5;
  Vec3 gravity{0,0,-9.81};
  bool tool_enabled=false;
  Vec3 tool_center{};
  double tool_radius=.5;
};
struct FluidResultV23 {
  std::vector<FluidParticleV23> particles;
  double peak_compression=0;
  double mass=0;
};
inline FluidResultV23 step_fluid_v23(const std::vector<FluidParticleV23> &input,
                                    const FluidSettingsV23 &s)
{
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  const auto positive=[](double x){return std::isfinite(x)&&x>0;};
  if(!positive(s.support)||!positive(s.mass)||!positive(s.rest_density)||
     !positive(s.dt)||!positive(s.relaxation)||!std::isfinite(s.viscosity)||
     s.viscosity<0||s.viscosity>1||s.iterations<1||s.iterations>32||
     !std::isfinite(norm(s.gravity))||
     (s.tool_enabled&&(!positive(s.tool_radius)||!std::isfinite(norm(s.tool_center)))))
    throw std::invalid_argument("Invalid fluid settings");
  constexpr double pi=3.14159265358979323846;
  const double h=s.support;
  const double w0=315/(64*pi*h*h*h);
  const auto kernel=[&](double d){
    const double x=std::max(0.0,1-d*d/(h*h));return w0*x*x*x;
  };
  const auto gradient=[&](Vec3 d,double len){
    if(len<=1e-12||len>=h)return Vec3{};
    const double x=1-len/h;
    return d*(-45/(pi*h*h*h*h)*x*x/len);
  };
  if(!positive(w0))throw std::overflow_error("Fluid kernel overflow");
  using Key=std::array<int,3>;
  std::vector<Vec3> p,correction(input.size());
  p.reserve(input.size());
  const auto contact=[&](Vec3 pos) {
    if(!s.tool_enabled)return pos;
    const auto d=pos-s.tool_center;const double len=norm(d);
    if(len>=s.tool_radius)return pos;
    return s.tool_center+(len>1e-12?d/len:Vec3{0,0,1})*s.tool_radius;
  };
  for(const auto &v:input) {
    if(!std::isfinite(norm(v.position))||!std::isfinite(norm(v.velocity))||
       norm(v.velocity+s.gravity*s.dt)*s.dt>.5*h)
      throw std::invalid_argument("Fluid trial requires smaller step");
    p.push_back(contact(v.position+(v.velocity+s.gravity*s.dt)*s.dt));
  }
  struct Pair {std::size_t a,b;double w;Vec3 grad;};
  std::vector<Pair> pairs;
  std::vector<double> rho(p.size()),lambda(p.size()),denom(p.size());
  std::vector<Vec3> grad_sum(p.size());
  const auto gather=[&]() {
    std::map<Key,std::vector<std::size_t>> grid;
    std::vector<Key> keys;
    keys.reserve(p.size());
    for(std::size_t i=0;i<p.size();++i) {
      const Vec3 x=p[i]/h;
      if(!std::isfinite(norm(x))||norm(x)>1e8)
        throw std::overflow_error("Fluid grid coordinate overflow");
      Key k{int(std::floor(x.x)),int(std::floor(x.y)),int(std::floor(x.z))};
      keys.push_back(k);grid[k].push_back(i);
    }
    pairs.clear();
    std::fill(rho.begin(),rho.end(),s.mass*w0);
    for(std::size_t i=0;i<p.size();++i) {
      const auto k=keys[i];
      for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z) {
        const auto it=grid.find({k[0]+x,k[1]+y,k[2]+z});
        if(it==grid.end())continue;
        for(auto j:it->second)if(j>i) {
          const Vec3 d=p[i]-p[j];const double len=norm(d);
          if(len>=h)continue;
          if(len<=1e-12)throw std::invalid_argument("Coincident fluid samples");
          const double w=kernel(len);
          const Vec3 g=gradient(d,len)*(s.mass/s.rest_density);
          pairs.push_back({i,j,w,g});
          rho[i]+=s.mass*w;rho[j]+=s.mass*w;
        }
      }
    }
  };
  for(int k=0;k<s.iterations;++k) {
    gather();
    std::fill(grad_sum.begin(),grad_sum.end(),Vec3{});
    std::fill(denom.begin(),denom.end(),0);
    std::fill(correction.begin(),correction.end(),Vec3{});
    for(const auto &e:pairs) {
      grad_sum[e.a]+=e.grad;grad_sum[e.b]+=e.grad*(-1);
      const double g=norm(e.grad);
      denom[e.a]+=g*g;denom[e.b]+=g*g;
    }
    for(std::size_t i=0;i<p.size();++i) {
      const double g=norm(grad_sum[i]);
      lambda[i]=-std::max(0.0,rho[i]/s.rest_density-1)/
                (denom[i]+g*g+s.relaxation);
    }
    for(const auto &e:pairs) {
      const Vec3 d=e.grad*(lambda[e.a]+lambda[e.b]);
      correction[e.a]+=d;correction[e.b]+=d*(-1);
    }
    double peak=0;
    for(const auto &d:correction) {
      if(!std::isfinite(norm(d)))throw std::overflow_error("Fluid correction overflow");
      peak=std::max(peak,norm(d));
    }
    const double scale=peak>.15*h?.15*h/peak:1;
    for(std::size_t i=0;i<p.size();++i)p[i]=contact(p[i]+correction[i]*scale);
  }
  gather();
  FluidResultV23 out;
  out.particles=input;out.mass=s.mass*double(input.size());
  if(!std::isfinite(out.mass))throw std::overflow_error("Fluid mass overflow");
  std::vector<Vec3> velocity(p.size()),dv(p.size());
  for(std::size_t i=0;i<p.size();++i) {
    velocity[i]=(p[i]-input[i].position)/s.dt;
    out.peak_compression=std::max(out.peak_compression,rho[i]/s.rest_density-1);
  }
  for(const auto &e:pairs) {
    const double alpha=s.viscosity*e.w*s.mass/std::max(rho[e.a],rho[e.b]);
    const Vec3 d=(velocity[e.b]-velocity[e.a])*alpha;
    dv[e.a]+=d;dv[e.b]+=d*(-1);
  }
  for(std::size_t i=0;i<p.size();++i) {
    out.particles[i]={p[i],velocity[i]+dv[i]};
    if(!std::isfinite(norm(out.particles[i].velocity)))
      throw std::overflow_error("Fluid velocity overflow");
  }
  return out;
}

} // namespace sculpt_physics
