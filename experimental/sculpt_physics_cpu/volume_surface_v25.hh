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
using VolumeTriangleV25=std::array<std::size_t,3>;
struct VolumeSurfaceV25 {
  std::vector<Vec3> positions;
  std::vector<VolumeTriangleV25> triangles;
};
inline double volume_dot_v25(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 volume_cross_v25(Vec3 a,Vec3 b){
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
inline double volume_norm_v25(Vec3 v){return std::hypot(v.x,v.y,v.z);}
inline void validate_closed_volume_v25(const VolumeSurfaceV25 &mesh){
  using Edge=std::array<std::size_t,2>;
  std::map<Edge,std::array<int,2>> incidence;
  if(mesh.triangles.empty())throw std::invalid_argument("Empty volume mesh");
  for(auto p:mesh.positions)if(!std::isfinite(volume_norm_v25(p)))
    throw std::invalid_argument("Invalid volume position");
  for(const auto &t:mesh.triangles){
    for(auto i:t)if(i>=mesh.positions.size())throw std::out_of_range("Volume triangle index");
    if(volume_norm_v25(volume_cross_v25(mesh.positions[t[1]]-mesh.positions[t[0]],
                                      mesh.positions[t[2]]-mesh.positions[t[0]]))<=1e-24)
      throw std::invalid_argument("Degenerate volume triangle");
    for(int k=0;k<3;++k){
      auto a=t[k],b=t[(k+1)%3];
      auto &e=incidence[{std::min(a,b),std::max(a,b)}];++e[a<b?0:1];
    }
  }
  for(const auto &e:incidence)if(e.second[0]!=1||e.second[1]!=1)
    throw std::invalid_argument("Volume requires closed consistently oriented triangle edges");
  // Edge manifold/orientation check only; not a self-intersection or
  // vertex-manifold test. Caller must supply a clean closed surface.
}
struct VolumeLatticeV25 {
  Vec3 origin{};
  double spacing=1;
  std::array<int,3> size{};
  std::size_t count=0;
  std::size_t index(int x,int y,int z)const{
    return (std::size_t(z)*size[1]+std::size_t(y))*size[0]+std::size_t(x);
  }
  Vec3 point(int x,int y,int z)const{return origin+Vec3{x*spacing,y*spacing,z*spacing};}
};
inline VolumeLatticeV25 volume_lattice_v25(const std::vector<Vec3>&p,double spacing,int pad,
                                          std::size_t budget=250000){
  if(p.empty()||!std::isfinite(spacing)||spacing<=1e-9||pad<0||pad>32||budget==0)
    throw std::invalid_argument("Invalid volume lattice");
  Vec3 lo=p.front(),hi=lo;
  for(auto v:p){
    if(!std::isfinite(volume_norm_v25(v)))throw std::invalid_argument("Invalid lattice point");
    lo={std::min(lo.x,v.x),std::min(lo.y,v.y),std::min(lo.z,v.z)};
    hi={std::max(hi.x,v.x),std::max(hi.y,v.y),std::max(hi.z,v.z)};
  }
  VolumeLatticeV25 l;l.spacing=spacing;l.origin=lo-Vec3{pad*spacing,pad*spacing,pad*spacing};
  l.count=1;const auto ext=hi-lo;
  const double a[3]={ext.x,ext.y,ext.z};
  for(int i=0;i<3;++i){
    double n=std::ceil(a[i]/spacing)+2*pad+2;
    if(!std::isfinite(n)||n>100000||n<2||n>double(budget/l.count))
      throw std::length_error("Volume lattice exceeds preview budget");
    l.size[i]=int(n);l.count*=std::size_t(l.size[i]);
  }
  return l;
}
/* Deterministic uniform interior sampling by signed solid angle. This
 * reference is deliberately bounded O(cells*triangles), not a production
 * BVH voxelizer. It cannot infer interior of open/ambiguous meshes. */
inline std::vector<Vec3> seed_volume_v25(const VolumeSurfaceV25 &mesh,double spacing,
                                       std::size_t max_particles=20000){
  validate_closed_volume_v25(mesh);
  auto l=volume_lattice_v25(mesh.positions,spacing,0);
  if(mesh.triangles.size()>50000000/l.count)
    throw std::length_error("Volume seeding work exceeds preview budget");
  std::vector<Vec3> out;
  constexpr double pi=3.14159265358979323846;
  for(int z=0;z<l.size[2];++z)for(int y=0;y<l.size[1];++y)for(int x=0;x<l.size[0];++x){
    const auto p=l.point(x,y,z)+Vec3{.5*spacing,.5*spacing,.5*spacing};
    double angle=0;
    for(const auto&t:mesh.triangles){
      const Vec3 a=mesh.positions[t[0]]-p,b=mesh.positions[t[1]]-p,c=mesh.positions[t[2]]-p;
      const double la=volume_norm_v25(a),lb=volume_norm_v25(b),lc=volume_norm_v25(c);
      angle+=2*std::atan2(volume_dot_v25(a,volume_cross_v25(b,c)),
          la*lb*lc+volume_dot_v25(a,b)*lc+volume_dot_v25(b,c)*la+volume_dot_v25(c,a)*lb);
    }
    if(std::abs(angle)>2*pi){
      if(out.size()>=max_particles)throw std::length_error("Volume particle budget exceeded");
      out.push_back(p);
    }
  }
  if(out.empty())throw std::invalid_argument("No interior samples: reduce spacing or inspect mesh");
  return out;
}
/* Surface of union of particle spheres, sampled on a bounded lattice.
 * Marching tetrahedra with a globally consistent cube diagonal and
 * shared edge vertices. Not a calibrated liquid density isosurface:
 * sphere radius determines bias in reconstructed volume. No UV transfer. */
inline VolumeSurfaceV25 extract_volume_v25(const std::vector<Vec3>&points,
                                           double spacing,double radius){
  if(!std::isfinite(spacing)||spacing<=1e-9||!std::isfinite(radius)||radius<=0||radius/spacing>8)
    throw std::invalid_argument("Invalid extraction radius");
  auto l=volume_lattice_v25(points,spacing,int(std::ceil(radius/spacing))+2);
  if(points.size()>20000)throw std::length_error("Extraction particle budget exceeded");
  const int reach=int(std::ceil(radius/spacing))+1;
  const std::size_t diameter=std::size_t(2*reach+1);
  if(points.size()>50000000/(diameter*diameter*diameter))
    throw std::length_error("Extraction splat work exceeds preview budget");
  std::vector<double> field(l.count,radius+spacing);
  for(auto p:points){
    const Vec3 q=(p-l.origin)/spacing;
    const int cx=int(std::floor(q.x)),cy=int(std::floor(q.y)),cz=int(std::floor(q.z));
    for(int z=std::max(0,cz-reach);z<=std::min(l.size[2]-1,cz+reach);++z)
      for(int y=std::max(0,cy-reach);y<=std::min(l.size[1]-1,cy+reach);++y)
        for(int x=std::max(0,cx-reach);x<=std::min(l.size[0]-1,cx+reach);++x){
          auto &f=field[l.index(x,y,z)];f=std::min(f,volume_norm_v25(l.point(x,y,z)-p)-radius);
        }
  }
  // Move exact iso-node ties slightly outside to avoid degenerate facets.
  for(auto &f:field)if(std::abs(f)<spacing*1e-8)f=spacing*1e-8;
  VolumeSurfaceV25 out;
  std::map<std::array<std::size_t,2>,std::size_t> edge_vertices;
  constexpr int tet[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},
                          {0,6,4,7},{0,4,5,7},{0,5,1,7}};
  for(int z=0;z<l.size[2]-1;++z)for(int y=0;y<l.size[1]-1;++y)
    for(int x=0;x<l.size[0]-1;++x){
      std::array<Vec3,8> p;std::array<std::size_t,8> id;
      int inside=0;
      for(int k=0;k<8;++k){
        const int xx=x+(k&1),yy=y+((k>>1)&1),zz=z+((k>>2)&1);
        p[k]=l.point(xx,yy,zz);id[k]=l.index(xx,yy,zz);
        inside+=field[id[k]]<0;
      }
      if(inside==0||inside==8)continue;
      const auto edge=[&](int a,int b){
        const std::array<std::size_t,2> key{std::min(id[a],id[b]),std::max(id[a],id[b])};
        const auto it=edge_vertices.find(key);if(it!=edge_vertices.end())return it->second;
        const double t=field[id[a]]/(field[id[a]]-field[id[b]]);
        const auto v=out.positions.size();out.positions.push_back(p[a]+(p[b]-p[a])*t);
        edge_vertices.emplace(key,v);return v;
      };
      for(const auto &t:tet){
        std::array<int,4> in{},ex{};int ni=0,ne=0;Vec3 a{},b{};
        for(int k:t)if(field[id[k]]<0){in[ni++]=k;a+=p[k];}else{ex[ne++]=k;b+=p[k];}
        if(ni==0||ne==0)continue;
        const Vec3 outward=b/double(ne)-a/double(ni);
        const auto emit=[&](std::size_t i,std::size_t j,std::size_t k){
          const auto n=volume_cross_v25(out.positions[j]-out.positions[i],out.positions[k]-out.positions[i]);
          if(volume_dot_v25(n,outward)<0)std::swap(j,k);
          out.triangles.push_back({i,j,k});
        };
        if(ni==1)emit(edge(in[0],ex[0]),edge(in[0],ex[1]),edge(in[0],ex[2]));
        else if(ne==1)emit(edge(in[0],ex[0]),edge(in[1],ex[0]),edge(in[2],ex[0]));
        else{
          const auto i=edge(in[0],ex[0]),j=edge(in[0],ex[1]),
                     k=edge(in[1],ex[0]),m=edge(in[1],ex[1]);
          emit(i,j,m);emit(i,m,k);
        }
      }
    }
  validate_closed_volume_v25(out);
  return out;
}
} // namespace sculpt_physics
