/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "sculpt_layer_delta_smooth_v01.hh"
#include "RNA_access.hh"
#include "BLI_listbase.h"

namespace blender::ed::sculpt_paint::layer_detail_v01 {
struct State {
  Object *object = nullptr;
  SubdivCCG *ccg = nullptr;
  Vector<layer_delta_smooth_v01::State> others;
  Map<int, float3> offsets;
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
  if (!ss || !ss->subdiv_ccg || !mmd || !layer_delta_smooth_v01::targets_layer(*mmd)) {
    return true;
  }
  const auto *active = bke::multires_layers::active(*mmd);
  if (!active || !bke::multires_layers::can_record(*mmd, *id_cast<Mesh *>(object.data)) ||
      ss->subdiv_ccg->level != mmd->totlvl || mmd->sculpt_displacement_enabled ||
      mmd->sculpt_shrinkwrap_enabled || brush.deform_target != BRUSH_DEFORM_TARGET_GEOMETRY ||
      !ELEM(brush.stroke_method, BRUSH_STROKE_SPACE, BRUSH_STROKE_ANCHORED))
  {
    BKE_report(reports, RPT_ERROR, "Preserve Other Layers: use Space or Anchored strokes at the highest Multires level, a visible active layer and no live deformers");
    return false;
  }
  State &s = state(); s.object = &object; s.ccg = ss->subdiv_ccg;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (layer.base_deltas) {
      for (int i=0; i<layer.base_verts_num; i++) {
        if (!is_zero_v3(layer.base_deltas[i])) {
          reset(); BKE_report(reports, RPT_ERROR, "Preserve Other Layers does not yet support L0 cage deltas");
          return false;
        }
      }
    }
    if (&layer == active || !bke::multires_layers::is_effective(layer)) { continue; }
    if (layer.level != s.ccg->level || !layer.disps) {
      reset();
      BKE_report(reports, RPT_ERROR, "Preserve Other Layers requires layers stored at the current Multires level");
      return false;
    }
    layer_delta_smooth_v01::State other;
    other.ccg=s.ccg; other.layer=&layer;
    other.ptex=bke::subdiv::face_ptex_offset_get(s.ccg->subdiv);
    other.stored_grid_size=s.ccg->grid_size; other.sample_stride=1;
    s.others.append(std::move(other));
  }
  return true;
}
inline bool capture(const int index)
{
  State &s=state(); if(s.offsets.contains(index)) { return true; }
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
  for(int alias:aliases) for(auto &other:s.others) {
    float3 offset;
    if(!layer_delta_smooth_v01::raw_offset(other,alias,offset)) { return false; }
    sum+=offset;
  }
  sum/=float(aliases.size());
  if(!layer_delta_smooth_v01::finite(sum)) { return false; }
  s.offsets.add(index,sum); return true;
}
inline bool prepare(Object &object,const IndexMask &mask)
{
  State &s=state(); s.active=false;
  if(s.object!=&object || s.others.is_empty()) { return true; }
  const auto &pbvh=*bke::object::pbvh_get(object);
  const auto nodes=pbvh.nodes<bke::pbvh::GridsNode>();
  const auto key=BKE_subdiv_ccg_key_top_level(*s.ccg);
  bool ok=true;
  mask.foreach_index([&](int n) {
    if(!ok) { return; }
    for(int grid:nodes[n].grids()) for(int j=0;j<key.grid_area;j++) {
      if(!capture(grid*key.grid_area+j)) { ok=false; return; }
    }
  },exec_mode::serial);
  s.active=ok; return ok;
}
/* Only adjust private brush scratch, never the live CCG or persistent layer data. */
inline void adjust(const SubdivCCG &ccg,Span<int> grids,MutableSpan<float3> positions)
{
  const State &s=state(); if(!s.active || s.ccg!=&ccg) { return; }
  for(int i:grids.index_range()) for(int j=0;j<ccg.grid_area;j++) {
    const auto *offset=s.offsets.lookup_ptr(grids[i]*ccg.grid_area+j);
    if(offset) { positions[i*ccg.grid_area+j]-=*offset; }
  }
}
inline void suspend() { state().active=false; }
/* Translate the anchor into the same reference surface as private position samples.
 * Restrict the search to captured nodes; never change the actual stroke anchor. */
inline float3 anchor_offset(const float3 &location)
{
  const State &s = state();
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
