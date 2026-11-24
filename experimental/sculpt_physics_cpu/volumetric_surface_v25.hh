/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* Bounded CPU reference particle surface extraction. Union of spherical
 * supports, sampled on a regular lattice and triangulated with a consistent
 * six-tetrahedron decomposition. Shared lattice edges share output vertices.
 * This is not an exact SDF, anisotropic liquid reconstruction, or UV transfer.
 * Source particle IDs identify provenance candidates, not original mesh corners.
 */
struct VolumeSampleV25 { Vec3 position{}; double radius=.1; };
struct VolumeSurfaceV25 {
  std::vector<Vec3> vertices;
  std::vector<std::array<std::size_t,3>> triangles;
  std::vector<std::size_t> nearest_particle;
};
inline VolumeSurfaceV25 extract_volume_surface_v25(
    const std::vector<VolumeSampleV25> &particles,double spacing,
    std::size_t max_nodes=1000000)
{
  VolumeSurfaceV25 out;
  if(!std::isfinite(spacing)||spacing<=1e-9||max_nodes<8)
    throw std::invalid_argument("Invalid extraction budget");
  if(particles.empty())return out;
  const auto norm=[](Vec3 p){return std::hypot(p.x,p.y,p.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  Vec3 lo=particles.front().position,hi=lo;
  for(const auto &p:particles){
    if(!std::isfinite(norm(p.position))||!std::isfinite(p.radius)||p.radius<=0)
      throw std::invalid_argument("Invalid extraction particle");
    lo.x=std::min(lo.x,p.position.x-p.radius);lo.y=std::min(lo.y,p.position.y-p.radius);lo.z=std::min(lo.z,p.position.z-p.radius);
    hi.x=std::max(hi.x,p.position.x+p.radius);hi.y=std::max(hi.y,p.position.y+p.radius);hi.z=std::max(hi.z,p.position.z+p.radius);
  }
  lo=lo-Vec3{spacing,spacing,spacing};hi=hi+Vec3{spacing,spacing,spacing};
  const auto count=[&](double x){
    const double n=std::ceil(x/spacing)+1;
    if(!std::isfinite(n)||n<2||n>double(max_nodes)||n>10000000)
      throw std::length_error("Volume lattice extent");
    return std::size_t(n);
  };
  const auto nx=count(hi.x-lo.x),ny=count(hi.y-lo.y),nz=count(hi.z-lo.z);
  if(nx>max_nodes/ny||nx*ny>max_nodes/nz)throw std::length_error("Volume lattice budget");
  const std::size_t n=nx*ny*nz;
  // Bound the naive reference evaluation independently of the lattice memory.
  if(particles.size()>100000000/n)throw std::length_error("Volume evaluation budget");
  const auto id=[&](std::size_t x,std::size_t y,std::size_t z){return (z*ny+y)*nx+x;};
  const auto pos=[&](std::size_t i){return lo+Vec3{double(i%nx)*spacing,double((i/nx)%ny)*spacing,double(i/(nx*ny))*spacing};};
  std::vector<double> field(n);
  for(std::size_t i=0;i<n;++i){
    double f=std::numeric_limits<double>::infinity();
    for(const auto &p:particles)f=std::min(f,norm(pos(i)-p.position)-p.radius);
    // Treat exact zeros as slightly outside to avoid collapsed edge vertices.
    field[i]=std::abs(f)<spacing*1e-9?spacing*1e-9:f;
  }
  std::map<std::array<std::size_t,2>,std::size_t> edges;
  const auto vertex=[&](std::size_t a,std::size_t b){
    if(a>b)std::swap(a,b);
    const std::array<std::size_t,2> k{a,b};
    const auto it=edges.find(k);if(it!=edges.end())return it->second;
    const double t=field[a]/(field[a]-field[b]);
    const Vec3 p=pos(a)+(pos(b)-pos(a))*t;
    const auto j=out.vertices.size();
    if(j>=max_nodes*8)throw std::length_error("Surface vertex budget");
    std::size_t nearest=0;double best=std::numeric_limits<double>::infinity();
    for(std::size_t i=0;i<particles.size();++i){
      const double d=std::abs(norm(p-particles[i].position)-particles[i].radius);
      if(d<best){best=d;nearest=i;}
    }
    out.vertices.push_back(p);out.nearest_particle.push_back(nearest);edges.emplace(k,j);
    return j;
  };
  const auto face=[&](std::size_t a,std::size_t b,std::size_t c,Vec3 outward){
    const Vec3 normal=cross(out.vertices[b]-out.vertices[a],out.vertices[c]-out.vertices[a]);
    if(norm(normal)==0)throw std::runtime_error("Degenerate extracted triangle");
    if(dot(normal,outward)<0)std::swap(b,c);
    if(out.triangles.size()>=max_nodes*16)throw std::length_error("Surface face budget");
    out.triangles.push_back({a,b,c});
  };
  constexpr int tet[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},{0,6,4,7},{0,4,5,7},{0,5,1,7}};
  for(std::size_t z=0;z+1<nz;++z)for(std::size_t y=0;y+1<ny;++y)for(std::size_t x=0;x+1<nx;++x){
    std::array<std::size_t,8> cube{};
    for(int k=0;k<8;++k)cube[k]=id(x+(k&1),y+((k>>1)&1),z+((k>>2)&1));
    for(const auto &t:tet){
      std::vector<std::size_t> in,ex;Vec3 ci{},ce{};
      for(int k:t){const auto i=cube[k];if(field[i]<0){in.push_back(i);ci+=pos(i);}else{ex.push_back(i);ce+=pos(i);}}
      if(in.empty()||ex.empty())continue;
      const Vec3 outward=ce/double(ex.size())-ci/double(in.size());
      if(in.size()==1)face(vertex(in[0],ex[0]),vertex(in[0],ex[1]),vertex(in[0],ex[2]),outward);
      else if(ex.size()==1)face(vertex(ex[0],in[0]),vertex(ex[0],in[1]),vertex(ex[0],in[2]),outward);
      else{
        const auto a=vertex(in[0],ex[0]),b=vertex(in[0],ex[1]),c=vertex(in[1],ex[1]),d=vertex(in[1],ex[0]);
        face(a,b,c,outward);face(a,c,d,outward);
      }
    }
  }
  return out;
}
} // namespace sculpt_physics
