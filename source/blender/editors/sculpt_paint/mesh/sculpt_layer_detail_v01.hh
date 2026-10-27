/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "sculpt_layer_delta_smooth_v01.hh"
#include "RNA_access.hh"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BKE_mesh.hh"

namespace blender::ed::sculpt_paint::layer_detail_v01 {
struct State {
  Object *object = nullptr;
  SubdivCCG *ccg = nullptr;
  const bke::multires_layers::SmoothL0Reference *protected_l0_reference = nullptr;
  Vector<layer_delta_smooth_v01::State> others;
  Array<float3> mesh_protected;
  bool mesh_reference_sampling = false, mesh_anchor_valid = false;
  float3 mesh_anchor_location = float3(0), mesh_anchor_offset = float3(0);
  /* Anchored retains the original map and traversal order for exact anchor ties. */
  Map<int, float3> offsets;
  layer_delta_smooth_v01::SampleStore compact_offsets;
  Set<int> prepared_grids;
  Vector<int> grid_owners;
  bool anchored = false, compact = false, reuse_prepared = true;
  bool grid_sample_ready = false;
  float3 grid_sample_location = float3(0), grid_sample_offset = float3(0);
  bool active = false;
};
inline State &state() { static State value; return value; }
inline void reset() { state() = State{}; }
inline bool supported(const Brush &brush)
{
  return ELEM(brush.sculpt_brush_type, SCULPT_BRUSH_TYPE_DRAW, SCULPT_BRUSH_TYPE_CLAY,
              SCULPT_BRUSH_TYPE_CLAY_STRIPS, SCULPT_BRUSH_TYPE_CLAY_THUMB, SCULPT_BRUSH_TYPE_INFLATE,
              SCULPT_BRUSH_TYPE_PLANE);
}
inline bool requested(const Brush &brush)
{
  if (!supported(brush)) { return false; }
  PointerRNA ptr = RNA_id_pointer_create(const_cast<ID *>(&brush.id));
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "sculpt_preserve_other_layers");
  return prop && RNA_property_boolean_get(&ptr, prop);
}
inline bool begin(Object &object, const Brush &brush, ReportList *reports,
                  const bool force_smooth = false)
{
  reset();
  if (force_smooth || !requested(brush)) { return true; }
  SculptSession *ss = object.runtime->sculpt_session;
  const MultiresModifierData *mmd = ss ? ss->multires_modifier : nullptr;
  if (ss && !ss->subdiv_ccg) {
    if (auto *base = bke::multires_layers::base_modifier(object)) {
      if (base->sculptlvl == 0 && layer_delta_smooth_v01::targets_layer(*base)) {
        State &s = state();
        if (!layer_delta_smooth_v01::mesh_protected_offsets(object, *base, s.mesh_protected, reports)) {
          reset(); return false;
        }
        s.object = &object;
        s.mesh_reference_sampling = true;
        return true;
      }
    }
  }
  if (!ss || !ss->subdiv_ccg || !mmd || !layer_delta_smooth_v01::targets_layer(*mmd)) {
    return true;
  }
  const auto *active = bke::multires_layers::active(*mmd);
  if (!active || !bke::multires_layers::can_record(*mmd, *id_cast<Mesh *>(object.data))) {
    BKE_report(reports, RPT_ERROR, "Preserve Other Layers requires a valid visible active layer with nonzero strength");
    return false;
  }
  bool has_other_layers = false;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (&layer != active && bke::multires_layers::is_effective(layer)) {
      has_other_layers = true;
      break;
    }
  }
  /* With no other contribution, ordinary sculpting already preserves everything.
   * Do not impose isolation-only stroke/level/cage restrictions on that case. */
  if (!has_other_layers) { return true; }
  if (ss->subdiv_ccg->level != mmd->totlvl || mmd->sculpt_displacement_enabled ||
      mmd->sculpt_shrinkwrap_enabled || brush.deform_target != BRUSH_DEFORM_TARGET_GEOMETRY ||
      !ELEM(brush.stroke_method, BRUSH_STROKE_SPACE, BRUSH_STROKE_ANCHORED))
  {
    BKE_report(reports, RPT_ERROR, "Preserve Other Layers: use Space or Anchored strokes at the highest Multires level, a visible active layer and no live deformers");
    return false;
  }
  State &s = state(); s.object = &object; s.ccg = ss->subdiv_ccg;
  s.anchored = brush.stroke_method == BRUSH_STROKE_ANCHORED;
  const char *compact = std::getenv("BLENDER_SCULPT_DETAIL_COMPACT_BUFFERS");
  const char *reuse = std::getenv("BLENDER_SCULPT_DETAIL_PREPARE_REUSE");
  s.compact = s.ccg->grid_area >= 25 && (!compact || compact[0] != '0');
  s.reuse_prepared = !reuse || reuse[0] != '0';
  if (s.compact && !s.anchored) { s.compact_offsets.configure(s.ccg->grid_area); }

  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    /* Hidden and zero-strength other layers do not affect the sampled surface. */
    if (&layer != active && !bke::multires_layers::is_effective(layer)) { continue; }
    if (layer.base_deltas) {
      for (int i=0; i<layer.base_verts_num; i++) {
        if (!is_zero_v3(layer.base_deltas[i])) {
          if (mmd->modifier.prev != nullptr) {
            reset(); BKE_report(reports, RPT_ERROR, "Preserve Other Layers with L0 edits requires Multires first in the modifier stack");
            return false;
          }
          break;
        }
      }
    }
    if (&layer == active) { continue; }
    if (layer.level != s.ccg->level || !layer.disps) {
      reset();
      BKE_report(reports, RPT_ERROR, "Preserve Other Layers requires layers stored at the current Multires level");
      return false;
    }
    layer_delta_smooth_v01::State other;
    other.ccg=s.ccg; other.layer=&layer;
    other.ptex=bke::subdiv::face_ptex_offset_get(s.ccg->subdiv);
    other.stored_grid_size=s.ccg->grid_size; other.sample_stride=1;
    if (s.compact) { other.raw_offsets.configure(s.ccg->grid_area); }
    s.others.append(std::move(other));
  }
  if (!bke::multires_layers::detail_l0_reference_prepare(
          *mmd, *active, *id_cast<Mesh *>(object.data), *s.ccg, s.protected_l0_reference)) {
    reset(); BKE_report(reports, RPT_ERROR, "Cannot prepare protected L0 layer reference");
    return false;
  }
  return true;
}
inline bool capture(const int index)
{
  State &s=state();
  if (s.compact && !s.anchored ? s.compact_offsets.contains(index) : s.offsets.contains(index)) {
    return true;
  }
  const CCGKey key=BKE_subdiv_ccg_key_top_level(*s.ccg);
  const auto coord=SubdivCCGCoord::from_index(key,index);
  Vector<int,16> aliases; aliases.append(index);
  if(coord.x==0 || coord.y==0 || coord.x==key.grid_size-1 || coord.y==key.grid_size-1) {
    SubdivCCGNeighbors neighbors;
    BKE_subdiv_ccg_neighbor_coords_get(*s.ccg,coord,true,neighbors);
    for(const auto duplicate:neighbors.duplicates()) { aliases.append(duplicate.to_index(key)); }
  }
  std::sort(aliases.begin(),aliases.end());
  float3 sum(0);
  for (int alias : aliases) {
    float3x3 tangent;
    bool tangent_ready = false;
    for (auto &other : s.others) {
      float3 offset;
      if (!layer_delta_smooth_v01::raw_offset(other, alias, offset, &tangent, &tangent_ready)) {
        return false;
      }
      sum += offset;
    }
    if (s.protected_l0_reference) {
      const auto sample = SubdivCCGCoord::from_index(key, alias);
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*s.ccg, sample.grid_index);
      const IndexRange face = s.ccg->faces[face_index];
      const int corner = sample.grid_index - face.start();
      const float gu = float(sample.x) / float(key.grid_size - 1);
      const float gv = float(sample.y) / float(key.grid_size - 1);
      float u, v;
      const bool quad = face.size() == 4;
      if (quad) { bke::subdiv::rotate_grid_to_quad(corner, gu, gv, &u, &v); }
      else { bke::subdiv::grid_uv_to_ptex_face_uv(gu, gv, &u, &v); }
      const int ptex = bke::subdiv::face_ptex_offset_get(s.ccg->subdiv)[face_index] +
                       (quad ? 0 : corner);
      sum += bke::multires_layers::smooth_l0_reference_sample(
          *s.protected_l0_reference, ptex, u, v);
    }
  }
  sum/=float(aliases.size());
  if(!layer_delta_smooth_v01::finite(sum)) { return false; }
  if (s.compact && !s.anchored) { s.compact_offsets.add(index, sum); }
  else { s.offsets.add(index, sum); }
  return true;
}
inline bool prepare(Object &object,const IndexMask &mask)
{
  State &s=state(); s.active=false;
  if (s.object == &object && s.mesh_reference_sampling && !s.mesh_protected.is_empty()) {
    const float3 location = object.runtime->sculpt_session->cache->location_symm;
    if (!s.mesh_anchor_valid || location != s.mesh_anchor_location) {
      const Mesh &mesh = *id_cast<Mesh *>(object.data);
      const auto *mmd = bke::multires_layers::base_modifier(object);
      const auto *active = mmd ? bke::multires_layers::active(*mmd) : nullptr;
      if (!active) { return false; }
      const auto &pbvh = *bke::object::pbvh_get(object);
      const auto nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      Set<int> faces;
      const auto vertex_faces = mesh.vert_to_face_map();
      mask.foreach_index([&](int n) {
        for (int vertex : nodes[n].verts()) { for (int face : vertex_faces[vertex]) { faces.add(face); } }
      }, exec_mode::serial);
      const auto base = mesh.vert_positions();
      const auto cv = mesh.corner_verts(); const auto triangles = mesh.corner_tris();
      const auto position = [&](int vertex) {
        float3 co = base[vertex] + s.mesh_protected[vertex];
        if (active->base_deltas && active->base_verts_num == base.size()) {
          co += float3(active->base_deltas[vertex]) * active->strength;
        }
        return co;
      };
      float closest = std::numeric_limits<float>::max();
      float3 offset(0);
      for (int face : faces) {
        for (int ti : bke::mesh::face_triangles_range(mesh.faces(), face)) {
          const int3 vertices(cv[triangles[ti].x], cv[triangles[ti].y], cv[triangles[ti].z]);
          const float3 a = position(vertices.x), b = position(vertices.y), c = position(vertices.z);
          float3 point;
          closest_on_tri_to_point_v3(point, location, a, b, c);
          const float distance = math::distance_squared(point, location);
          if (distance >= closest) { continue; }
          const float3 ab=b-a, ac=c-a, ap=point-a;
          const float d00=math::dot(ab,ab), d01=math::dot(ab,ac), d11=math::dot(ac,ac);
          const float denominator=d00*d11-d01*d01;
          if (std::abs(denominator)<1e-20f) { continue; }
          const float v=(d11*math::dot(ap,ab)-d01*math::dot(ap,ac))/denominator;
          const float w=(d00*math::dot(ap,ac)-d01*math::dot(ap,ab))/denominator;
          offset=(1-v-w)*s.mesh_protected[vertices.x]+v*s.mesh_protected[vertices.y]+w*s.mesh_protected[vertices.z];
          closest=distance;
        }
      }
      if (!std::isfinite(closest) || closest==std::numeric_limits<float>::max() ||
          !layer_delta_smooth_v01::finite(offset)) { return false; }
      s.mesh_anchor_location=location;s.mesh_anchor_offset=offset;s.mesh_anchor_valid=true;
    }
    s.active=true;return true;
  }
  if(s.object!=&object || s.others.is_empty()) { return true; }
  const auto &pbvh=*bke::object::pbvh_get(object);
  const auto nodes=pbvh.nodes<bke::pbvh::GridsNode>();
  const auto key=BKE_subdiv_ccg_key_top_level(*s.ccg);
  bool ok=true;
  mask.foreach_index([&](int n) {
    if(!ok) { return; }
    for (const int grid : nodes[n].grids()) {
      if (s.reuse_prepared && s.prepared_grids.contains(grid)) { continue; }
      if (s.compact) {
        if (!s.anchored) { s.compact_offsets.prepare_grid(grid); }
        for (auto &other : s.others) { other.raw_offsets.prepare_grid(grid); }
      }
      for (int j = 0; j < key.grid_area; j++) {
        if (!capture(grid * key.grid_area + j)) { ok = false; return; }
      }
      if (s.reuse_prepared) { s.prepared_grids.add(grid); }
    }
  },exec_mode::serial);
  if (ok && !s.anchored) {
    const float3 location = object.runtime->sculpt_session->cache->location_symm;
    if (!s.grid_sample_ready || location != s.grid_sample_location) {
      Map<int, const float3 *> original_grids;
      if (!object.runtime->sculpt_session->cache->accum) {
        mask.foreach_index([&](int n) {
          const auto original=orig_position_data_lookup_grids(object,nodes[n]);
          if (!original || original->positions.size()!=nodes[n].grids().size()*key.grid_area) { return; }
          for (const int i:nodes[n].grids().index_range()) {
            original_grids.add(nodes[n].grids()[i],original->positions.data()+i*key.grid_area);
          }
        },exec_mode::serial);
      }
      const auto sample_position = [&](int index) -> float3 {
        if (const float3 *const *positions=original_grids.lookup_ptr(index/key.grid_area)) {
          return (*positions)[index%key.grid_area];
        }
        return s.ccg->positions[index];
      };
      float closest = std::numeric_limits<float>::max();
      float3 sample_offset(0);
      const auto test_cell = [&](const SubdivCCGCoord &coord) {
        const int side = key.grid_size;
        for (int y=std::max(0,int(coord.y)-1);y<=std::min(side-2,int(coord.y));y++) {
          for (int x=std::max(0,int(coord.x)-1);x<=std::min(side-2,int(coord.x));x++) {
            const int a=coord.grid_index*key.grid_area+y*side+x;
            for (const int3 triangle : {int3(a,a+1,a+side+1),int3(a,a+side+1,a+side)}) {
              const float3 p=sample_position(triangle.x),q=sample_position(triangle.y),r=sample_position(triangle.z);
              float3 point;closest_on_tri_to_point_v3(point,location,p,q,r);
              const float distance=math::distance_squared(point,location);
              if (distance>=closest) { continue; }
              const float3 pq=q-p,pr=r-p,pp=point-p;
              const float d00=math::dot(pq,pq),d01=math::dot(pq,pr),d11=math::dot(pr,pr);
              const float denominator=d00*d11-d01*d01;
              if (std::abs(denominator)<1e-24f) { continue; }
              if (!capture(triangle.x) || !capture(triangle.y) || !capture(triangle.z)) { continue; }
              const auto offset = [&](int i) -> float3 {
                if (s.compact) { return *s.compact_offsets.lookup_ptr(i); }
                return s.offsets.lookup(i);
              };
              const float v=(d11*math::dot(pp,pq)-d01*math::dot(pp,pr))/denominator;
              const float w=(d00*math::dot(pp,pr)-d01*math::dot(pp,pq))/denominator;
              sample_offset=(1-v-w)*offset(triangle.x)+v*offset(triangle.y)+w*offset(triangle.z);
              closest=distance;
            }
          }
        }
      };
      const auto test_vertex = [&](int index) {
        const auto coord=SubdivCCGCoord::from_index(key,index);test_cell(coord);
        if (coord.x==0 || coord.y==0 || coord.x==key.grid_size-1 || coord.y==key.grid_size-1) {
          SubdivCCGNeighbors neighbors;BKE_subdiv_ccg_neighbor_coords_get(*s.ccg,coord,true,neighbors);
          for (const auto duplicate : neighbors.duplicates()) { test_cell(duplicate); }
        }
      };
      /* Native cursor raycast already identifies the grid vertex: constant local
       * work in the common case, no search over the whole dense surface. */
      const int active=object.runtime->sculpt_session->active_vert_index();
      if (active>=0 && active<s.ccg->positions.size()) { test_vertex(active); }
      const float epsilon=std::max(1e-6f,object.runtime->sculpt_session->cache->radius*1e-4f);
      if (closest>epsilon*epsilon) {
        /* Replay strokes or mirrored passes may not have a matching cursor vertex.
         * Search only the current brush nodes, then interpolate their local cell. */
        float nearest=std::numeric_limits<float>::max();int index=-1;
        mask.foreach_index([&](int n) {
          for (int grid:nodes[n].grids()) for (int j=0;j<key.grid_area;j++) {
            const int i=grid*key.grid_area+j;const float distance=math::distance_squared(sample_position(i),location);
            if (distance<nearest) { nearest=distance;index=i; }
          }
        },exec_mode::serial);
        if (index>=0) { test_vertex(index); }
      }
      if (closest==std::numeric_limits<float>::max() || !layer_delta_smooth_v01::finite(sample_offset)) { return false; }
      s.grid_sample_location=location;s.grid_sample_offset=sample_offset;s.grid_sample_ready=true;
    }
  }
  s.active=ok; return ok;
}
/* Bounds are in the composed frame; private brush factors can reach a shared
 * sample outside the original node mask. Close that local region across grid
 * duplicates before Undo capture and writes, not by averaging them at Commit. */
inline IndexMask extend_shared_region(Object &object, const Brush &brush,
                                      const IndexMask &initial, IndexMaskMemory &memory)
{
  State &s=state();
  if (s.object!=&object || !s.ccg || s.anchored || s.others.is_empty() || initial.is_empty()) {
    return initial;
  }
  const auto &pbvh=*bke::object::pbvh_get(object);
  const auto nodes=pbvh.nodes<bke::pbvh::GridsNode>();
  const auto key=BKE_subdiv_ccg_key_top_level(*s.ccg);
  if (s.grid_owners.size()!=s.ccg->grids_num) {
    s.grid_owners.resize(s.ccg->grids_num);s.grid_owners.as_mutable_span().fill(-1);
    const IndexMask leaves=bke::pbvh::all_leaf_nodes(pbvh,memory);
    leaves.foreach_index([&](int n){for(int grid:nodes[n].grids()){s.grid_owners[grid]=n;}});
  }
  BitVector<> included(nodes.size(),false);
  Vector<int> frontier;
  initial.foreach_index([&](int n){included[n].set();frontier.append(n);});
  const auto &cache=*object.runtime->sculpt_session->cache;
  const bool plane_footprint=ELEM(brush.sculpt_brush_type,SCULPT_BRUSH_TYPE_CLAY_STRIPS,
                                  SCULPT_BRUSH_TYPE_CLAY_THUMB,SCULPT_BRUSH_TYPE_PLANE);
  const float support_squared=(plane_footprint?4.0f:1.0f)*cache.radius*cache.radius;
  const float3 view=math::normalize(cache.view_normal_symm);
  while (!frontier.is_empty()) {
    std::sort(frontier.begin(),frontier.end());
    const IndexMask current=IndexMask::from_indices(frontier.as_span(),memory);
    if (!prepare(object,current)) {return IndexMask();}
    const float3 center=cache.location_symm-s.grid_sample_offset;
    Vector<int> next;
    const auto test = [&](const int index, const float3 &point) {
      const auto *offset=s.compact?s.compact_offsets.lookup_ptr(index):s.offsets.lookup_ptr(index);
      if (!offset) {return;}
      const float3 relative=point-*offset-center;
      const float3 projected=relative-view*math::dot(relative,view);
      if (math::length_squared(projected)>support_squared) {return;}
      const auto coord=SubdivCCGCoord::from_index(key,index);
      SubdivCCGNeighbors neighbors;BKE_subdiv_ccg_neighbor_coords_get(*s.ccg,coord,true,neighbors);
      for(const auto duplicate:neighbors.duplicates()) {
        const int owner=s.grid_owners[duplicate.grid_index];
        if(owner>=0 && !included[owner]) {included[owner].set();next.append(owner);}
      }
    };
    for (const int n:frontier) {
      const auto original=!cache.accum?orig_position_data_lookup_grids(object,nodes[n]):std::nullopt;
      int grid_slot=0;
      for (const int grid:nodes[n].grids()) {
        const auto sample = [&](const int local) {
          const int index=grid*key.grid_area+local;
          test(index,s.ccg->positions[index]);
          if (original && original->positions.size()==nodes[n].grids().size()*key.grid_area) {
            test(index,original->positions[grid_slot*key.grid_area+local]);
          }
        };
        for (int x=0;x<key.grid_size;x++) {sample(x);sample((key.grid_size-1)*key.grid_size+x);}
        for (int y=1;y<key.grid_size-1;y++) {sample(y*key.grid_size);sample(y*key.grid_size+key.grid_size-1);}
        grid_slot++;
      }
    }
    frontier=std::move(next);
  }
  return IndexMask::from_predicate(nodes.index_range(),memory,[&](int n){return included[n];},exec_mode::serial);
}
inline bool sampling_offset_needed(const Brush &brush)
{
  const State &s=state();
  return s.active && (s.mesh_reference_sampling ||
                     ELEM(brush.stroke_method,BRUSH_STROKE_SPACE,BRUSH_STROKE_ANCHORED));
}
/* Only adjust private brush scratch, never the live CCG or persistent layer data. */
inline void adjust(const SubdivCCG &ccg,Span<int> grids,MutableSpan<float3> positions)
{
  const State &s=state(); if(!s.active || s.ccg!=&ccg) { return; }
  for (const int i : grids.index_range()) {
    const float3 *dense = s.compact && !s.anchored ?
                             s.compact_offsets.prepared_grid_data(grids[i]) : nullptr;
    if (dense) {
      for (int j = 0; j < ccg.grid_area; j++) { positions[i * ccg.grid_area + j] -= dense[j]; }
    }
    else {
      for (int j = 0; j < ccg.grid_area; j++) {
        const auto *offset = s.offsets.lookup_ptr(grids[i] * ccg.grid_area + j);
        if (offset) { positions[i * ccg.grid_area + j] -= *offset; }
      }
    }
  }
}
inline void suspend() { state().active=false; }
/* Translate the anchor into the same reference surface as private position samples.
 * Restrict the search to captured nodes; never change the actual stroke anchor. */
inline float3 anchor_offset(const float3 &location)
{
  const State &s = state();
  if (s.mesh_reference_sampling && s.mesh_anchor_valid) { return s.mesh_anchor_offset; }
  if (s.ccg && !s.anchored && s.grid_sample_ready) { return s.grid_sample_offset; }
  float closest = std::numeric_limits<float>::max();
  float3 result(0);
  if (!s.active) { return result; }
  for (const auto item : s.offsets.items()) {
    const float distance = math::distance_squared(s.ccg->positions[item.key], location);
    if (distance < closest) { closest = distance; result = item.value; }
  }
  return result;
}
} // namespace blender::ed::sculpt_paint::layer_detail_v01
