/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "physics_core.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace sculpt_physics {

/* Transactional triangle-patch reference, never edits Blender. Corner UVs
 * remain per-face: interpolating across a seam would destroy the UV layout.
 * Source vertex IDs in provenance are local to the supplied input patch.
 * Blender requires a complete adapter for attributes, BMesh/PBVH and Undo. */
struct MaterialPatchVertex {
  Vec3 position{};
  double mask=0.0;
  std::array<std::size_t,2> source{};
  double weight=0.0;
};
struct MaterialPatchFace {
  std::array<std::size_t,3> vertex{};
  std::array<UV2,3> uv{};
  int face_set=0;
  int material=0;
};
struct MaterialPatch {
  std::vector<MaterialPatchVertex> vertices;
  std::vector<MaterialPatchFace> faces;
};

inline void validate_material_patch(const MaterialPatch &p)
{
  for(const auto &v:p.vertices)
    if(!std::isfinite(std::hypot(v.position.x,v.position.y,v.position.z)) ||
       !std::isfinite(v.mask) || v.mask<0 || v.mask>1)
      throw std::invalid_argument("Invalid patch vertex");
  for(const auto &f:p.faces) {
    for(auto id:f.vertex) if(id>=p.vertices.size())
      throw std::out_of_range("Patch corner index");
    if(f.vertex[0]==f.vertex[1] || f.vertex[1]==f.vertex[2] || f.vertex[2]==f.vertex[0])
      throw std::invalid_argument("Repeated patch corner");
    for(const auto uv:f.uv)
      if(!std::isfinite(uv.u)||!std::isfinite(uv.v))
        throw std::invalid_argument("Invalid patch UV");
  }
}

/* Split every triangle sharing the requested edge, preserving winding,
 * face-set/material and separate UV coordinates on each side of seams.
 * This really creates a new vertex in the staged reference patch. */
inline MaterialPatch split_material_edge(const MaterialPatch &p,std::size_t a,std::size_t b)
{
  validate_material_patch(p);
  if(a>=p.vertices.size() || b>=p.vertices.size() || a==b)
    throw std::invalid_argument("Invalid split edge");
  if(p.vertices[a].mask>=0.999 || p.vertices[b].mask>=0.999)
    throw std::invalid_argument("Cannot split fully masked edge");
  MaterialPatch out=p;
  out.faces.clear();
  const auto id=out.vertices.size();
  out.vertices.push_back({(p.vertices[a].position+p.vertices[b].position)*0.5,
      (p.vertices[a].mask+p.vertices[b].mask)*0.5,{a,b},0.5});
  std::size_t incident=0;
  for(const auto &f:p.faces) {
    bool found=false;
    for(int k=0;k<3;++k) {
      const int j=(k+1)%3,m=(k+2)%3;
      if(!((f.vertex[k]==a && f.vertex[j]==b) || (f.vertex[k]==b && f.vertex[j]==a))) continue;
      ++incident;
      const UV2 uv{(f.uv[k].u+f.uv[j].u)*0.5,(f.uv[k].v+f.uv[j].v)*0.5};
      auto left=f,right=f;
      left.vertex={f.vertex[k],id,f.vertex[m]};
      left.uv={f.uv[k],uv,f.uv[m]};
      right.vertex={id,f.vertex[j],f.vertex[m]};
      right.uv={uv,f.uv[j],f.uv[m]};
      out.faces.push_back(left);out.faces.push_back(right);
      found=true;break;
    }
    if(!found) out.faces.push_back(f);
  }
  if(incident==0 || incident>2)
    throw std::invalid_argument("Missing or nonmanifold split edge");
  return out;
}

/* Explicit selected-region separation prototype: duplicate vertices on
 * its shared boundary, preserving face corners and original coordinates.
 * A real topological separation, not an opened-looking displacement.
 * No crack propagation, wall generation, watertightness or Multires
 * reconstruction; caller must supply a coherent face region. */
inline MaterialPatch separate_material_region(const MaterialPatch &p,
                                               const std::vector<bool> &selected)
{
  validate_material_patch(p);
  if(selected.size()!=p.faces.size())
    throw std::invalid_argument("Invalid separation face selection");
  std::vector<bool> inside(p.vertices.size(),false),outside(p.vertices.size(),false);
  for(std::size_t f=0;f<p.faces.size();++f)
    for(auto v:p.faces[f].vertex) {
      if(selected[f]) inside[v]=true; else outside[v]=true;
    }
  MaterialPatch out=p;
  std::vector<std::size_t> map(p.vertices.size());
  for(std::size_t i=0;i<p.vertices.size();++i) {
    map[i]=i;
    if(inside[i] && outside[i]) {
      if(p.vertices[i].mask>=0.999) throw std::invalid_argument("Masked separation boundary");
      map[i]=out.vertices.size();
      auto v=p.vertices[i];v.source={i,i};v.weight=0;
      out.vertices.push_back(v);
    }
  }
  for(std::size_t f=0;f<p.faces.size();++f) if(selected[f])
    for(auto &v:out.faces[f].vertex) v=map[v];
  return out;
}

} // namespace sculpt_physics
