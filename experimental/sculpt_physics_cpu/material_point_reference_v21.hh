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

/* Explicit 3D CPU material-point reference: trilinear grid transfer, internal
 * stress force, PIC velocity update, compressible pressure and small-strain
 * deviatoric elastoplastic stress. Not a production large-deformation MPM
 * implementation (no APIC, objective stress rate, implicit solve, surface
 * extraction or CUDA). Tool collision acts on grid velocity, not triangle
 * self-collision. All inputs immutable; caller owns commit/Undo. */
using MaterialMatrix=std::array<std::array<double,3>,3>;
struct MaterialPointV21 {
  Vec3 position{},velocity{};
  double mass=1.0,rest_volume=1.0,volume=1.0;
  MaterialMatrix deviator{};
  double plastic_strain=0.0;
};
struct MaterialPointSettingsV21 {
  double spacing=1.0,dt=0.01;
  double bulk=20.0,shear=8.0,cohesion=0.5,friction=0.5,viscosity=0.1;
  Vec3 gravity{};
};
struct MaterialPointToolV21 {
  bool enabled=false;
  Vec3 center{},velocity{};
  double radius=1.0,friction=0.5;
};
struct MaterialPointStepV21 {
  std::vector<MaterialPointV21> particles;
  double mass=0.0,grid_mass=0.0;
  std::size_t grid_nodes=0,contact_nodes=0;
};

inline MaterialPointStepV21 step_material_points_v21(
    const std::vector<MaterialPointV21> &points,
    const MaterialPointSettingsV21 &s,
    const MaterialPointToolV21 &tool={})
{
  const auto norm=[](Vec3 v){return std::hypot(v.x,v.y,v.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto component=[](Vec3 v,int i){return i==0?v.x:i==1?v.y:v.z;};
  const auto positive=[](double v){return std::isfinite(v)&&v>0;};
  const auto nonnegative=[](double v){return std::isfinite(v)&&v>=0;};
  if(!positive(s.spacing)||!positive(s.dt)||!nonnegative(s.bulk)||
     !nonnegative(s.shear)||!nonnegative(s.cohesion)||!nonnegative(s.friction)||
     !nonnegative(s.viscosity)||!std::isfinite(norm(s.gravity))||
     (tool.enabled&&(!positive(tool.radius)||!nonnegative(tool.friction)||
                     !std::isfinite(norm(tool.center))||!std::isfinite(norm(tool.velocity)))))
    throw std::invalid_argument("Invalid material point settings");
  using Key=std::array<int,3>;
  struct Node {double mass=0;Vec3 momentum{},force{},velocity{};};
  std::map<Key,Node> grid;
  const auto visit=[&](Vec3 position,auto &&fn) {
    Key base{};std::array<double,3> f{};
    for(int a=0;a<3;++a) {
      const double x=component(position,a)/s.spacing;
      if(!std::isfinite(x)||std::abs(x)>1e8)throw std::out_of_range("MPM grid position");
      base[a]=int(std::floor(x));f[a]=x-base[a];
    }
    for(int i=0;i<2;++i)for(int j=0;j<2;++j)for(int k=0;k<2;++k) {
      const std::array<int,3> bit{i,j,k};
      Key key{base[0]+i,base[1]+j,base[2]+k};
      std::array<double,3> w{};
      for(int a=0;a<3;++a)w[a]=bit[a]?f[a]:1-f[a];
      const double weight=w[0]*w[1]*w[2];
      const Vec3 gradient{(i?1.0:-1.0)*w[1]*w[2]/s.spacing,
                          (j?1.0:-1.0)*w[0]*w[2]/s.spacing,
                          (k?1.0:-1.0)*w[0]*w[1]/s.spacing};
      fn(key,weight,gradient);
    }
  };
  MaterialPointStepV21 out;
  out.particles=points;
  for(const auto &p:points) {
    if(!std::isfinite(norm(p.position))||!std::isfinite(norm(p.velocity))||
       !positive(p.mass)||!positive(p.volume)||!positive(p.rest_volume)||
       !nonnegative(p.plastic_strain))throw std::invalid_argument("Invalid material point");
    const double rho=p.mass/p.volume;
    const double speed=norm(p.velocity)+std::sqrt((s.bulk+4*s.shear/3)/rho);
    if(!positive(rho)||!std::isfinite(speed)||s.dt*speed>s.spacing*0.25||
       s.dt*s.viscosity/rho>s.spacing*s.spacing*0.1)
      throw std::invalid_argument("Material point step exceeds explicit stability budget");
    const double pressure=-s.bulk*std::log(p.volume/p.rest_volume);
    MaterialMatrix stress=p.deviator;
    for(int i=0;i<3;++i)for(int j=0;j<3;++j) {
      if(!std::isfinite(stress[i][j]))throw std::invalid_argument("Invalid particle stress");
      if(i==j)stress[i][j]-=pressure;
    }
    out.mass+=p.mass;
    visit(p.position,[&](const Key &key,double w,Vec3 grad){
      auto &node=grid[key];node.mass+=w*p.mass;
      node.momentum+=p.velocity*(w*p.mass);
      Vec3 force{};
      for(int i=0;i<3;++i) {
        double a=0;
        for(int j=0;j<3;++j)a+=stress[i][j]*component(grad,j);
        if(i==0)force.x=a;else if(i==1)force.y=a;else force.z=a;
      }
      node.force+=force*(-p.volume);
    });
  }
  for(auto &entry:grid) {
    auto &n=entry.second;out.grid_mass+=n.mass;
    if(n.mass<=1e-15)continue;
    n.velocity=n.momentum/n.mass+(n.force/n.mass+s.gravity)*s.dt;
    if(tool.enabled) {
      const auto &key=entry.first;
      const Vec3 p{key[0]*s.spacing,key[1]*s.spacing,key[2]*s.spacing};
      const Vec3 offset=p-tool.center;
      const double d=norm(offset);
      if(d<tool.radius && d>1e-12) {
        const Vec3 normal=offset/d;
        const Vec3 relative=n.velocity-tool.velocity;
        const double vn=dot(relative,normal);
        if(vn<0) {
          const Vec3 tangent=relative-normal*vn;
          const double l=norm(tangent);
          const double slip=l>0?std::max(0.0,1.0-tool.friction*(-vn)/l):0.0;
          n.velocity=tool.velocity+tangent*slip;
          ++out.contact_nodes;
        }
      }
    }
    if(!std::isfinite(norm(n.velocity)))throw std::overflow_error("Invalid MPM grid velocity");
  }
  out.grid_nodes=grid.size();
  for(std::size_t i=0;i<points.size();++i) {
    auto &p=out.particles[i];
    MaterialMatrix L{};
    Vec3 velocity{};
    visit(points[i].position,[&](const Key &key,double w,Vec3 grad){
      const auto &n=grid.at(key);
      velocity+=n.velocity*w;
      for(int a=0;a<3;++a)for(int b=0;b<3;++b)
        L[a][b]+=component(n.velocity,a)*component(grad,b);
    });
    const double div=L[0][0]+L[1][1]+L[2][2];
    if(!std::isfinite(div)||std::abs(div*s.dt)>0.25||
       norm(velocity)*s.dt>s.spacing*0.5)
      throw std::runtime_error("MPM trial needs smaller substeps");
    p.velocity=velocity;p.position+=velocity*s.dt;
    p.volume*=std::exp(div*s.dt);
    const double pressure=-s.bulk*std::log(p.volume/p.rest_volume);
    double magnitude=0;
    for(int a=0;a<3;++a)for(int b=0;b<3;++b) {
      const double rate=0.5*(L[a][b]+L[b][a])-(a==b?div/3:0);
      // Viscoelastic relaxation, followed by pressure-dependent yield.
      p.deviator[a][b]=(p.deviator[a][b]+2*s.shear*s.dt*rate)/
                       (1+s.viscosity*s.dt);
      magnitude+=p.deviator[a][b]*p.deviator[a][b];
    }
    magnitude=std::sqrt(magnitude);
    const double yield=s.cohesion+s.friction*std::max(0.0,pressure);
    if(magnitude>yield && magnitude>0) {
      p.plastic_strain+=(magnitude-yield)/std::max(s.shear,1e-12);
      for(auto &row:p.deviator)for(auto &v:row)v*=yield/magnitude;
    }
    if(!positive(p.volume)||!std::isfinite(norm(p.position))||
       !nonnegative(p.plastic_strain)||!std::isfinite(magnitude))
      throw std::overflow_error("Invalid MPM trial");
  }
  if(!std::isfinite(out.mass)||!std::isfinite(out.grid_mass))
    throw std::overflow_error("MPM mass overflow");
  return out;
}

} // namespace sculpt_physics
