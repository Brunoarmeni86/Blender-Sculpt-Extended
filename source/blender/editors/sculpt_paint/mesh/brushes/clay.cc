/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"

#include "BKE_object_types.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"

#include "PRF_profile.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
#  include "physics_brush_v15.hh"
#endif
#include "physical_material_finish.hh"

namespace blender::ed::sculpt_paint::brushes {
inline namespace clay_cc {
struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

BLI_NOINLINE static void calc_closest_to_plane(const float4 &test_plane,
                                               const Span<float3> positions,
                                               const Span<int> verts,
                                               const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  /* Equivalent to #closest_to_plane_normalized_v3 */
  BLI_assert(verts.size() == translations.size());
  for (const int i : verts.index_range()) {
    const float side = plane_point_side_v3(test_plane, positions[verts[i]]);
    translations[i] = float3(test_plane) * -side;
  }
}

BLI_NOINLINE static void calc_closest_to_plane(const float4 &test_plane,
                                               const Span<float3> positions,
                                               const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  /* Equivalent to #closest_to_plane_normalized_v3 */
  BLI_assert(positions.size() == translations.size());
  for (const int i : positions.index_range()) {
    const float side = plane_point_side_v3(test_plane, positions[i]);
    translations[i] = float3(test_plane) * -side;
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float4 &test_plane,
                       const float strength,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Span<int> verts = node.verts();

  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   position_data.eval,
                                   vert_normals,
                                   node,
                                   tls.factors,
                                   tls.distances);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;

  calc_closest_to_plane(test_plane, position_data.eval, verts, translations);
  scale_translations(translations, strength);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4 &test_plane,
                       const float strength,
                       bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;

  calc_closest_to_plane(test_plane, positions, translations);
  scale_translations(translations, strength);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4 &test_plane,
                       const float strength,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;

  calc_closest_to_plane(test_plane, positions, translations);
  scale_translations(translations, strength);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace clay_cc

void do_clay_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &object,
                   const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  if (physics_v15::freeze_if_mode_changed(object,pbvh,brush)) return;
#endif

  float3 area_no;
  float3 area_co;
  calc_brush_plane(depsgraph, brush, object, node_mask, area_no, area_co);

  const float initial_radius = fabsf(ss.cache->initial_radius);
  const float offset = brush_plane_offset_get(brush, ss);

  /* This implementation skips a factor calculation as it currently has
   * no user-facing impact (i.e. is effectively a constant)
   * See: #123518 */
  float displace = fabsf(initial_radius * offset);

  const bool flip = ss.cache->bstrength < 0.0f;
  if (flip) {
    displace = -displace;
  }

  const float3 modified_area_co = ss.cache->location_symm + (area_no * ss.cache->scale * displace);

  float4 test_plane;
  plane_from_point_normal_v3(test_plane, modified_area_co, area_no);
  BLI_ASSERT_UNIT_V3(test_plane);

  const float bstrength = fabsf(ss.cache->bstrength);
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  if (physics_v15::requested(object,brush) && pbvh.type()==bke::pbvh::Type::Mesh) {
    const Mesh &mesh=*id_cast<Mesh *>(object.data);
    const MeshAttributeData attributes(mesh);
    const PositionDeformData pos(depsgraph,object,true);
    const Span<float3> normals=bke::pbvh::vert_normals_eval(depsgraph,object);
    MutableSpan<bke::pbvh::MeshNode> nodes=pbvh.nodes<bke::pbvh::MeshNode>();
    std::vector<sculpt_physics::NativeBrushTranslation> native;
    std::vector<sculpt_physics::PhysicalBrushIntentSample> intent_samples;
    const bool autonomous=physics_v15::autonomous_requested(*ss.cache);
    Vector<float> factors;
    Vector<float> distances;
    Vector<float3> translations;
    const bool footprint=autonomous && physics_v15::footprint_v30e_brush(object,brush);
    const bool audit=footprint && physics_v15::profile_v26b_requested();
    physics_v15::FootprintAuditV30E coverage;
    node_mask.foreach_index([&](const int ni) {
      const bke::pbvh::MeshNode &node=nodes[ni];
      const Span<int> verts=node.verts();
      if(footprint){
        factors.resize(verts.size());distances.resize(verts.size());
        for(const int i:verts.index_range()){
          distances[i]=physics_v15::footprint_distance_v30e(*ss.cache,brush,pos.eval[verts[i]],area_no);
          if(audit && distances[i]<ss.cache->radius)++coverage.geometric;
        }
        fill_factor_from_hide_and_mask(attributes.hide_vert,attributes.mask,verts,factors);
        filter_region_clip_factors(ss,pos.eval,verts,factors);
        if(audit)coverage.base+=coverage.count(factors,distances,ss.cache->radius);
        if(brush.flag & BRUSH_FRONTFACE)calc_front_face(ss.cache->view_normal_symm,normals,verts,factors);
        if(audit){coverage.front+=coverage.count(factors,distances,ss.cache->radius);coverage.trim=coverage.front;}
        filter_distances_with_radius(ss.cache->radius,distances,factors);
        apply_hardness_to_distances(ss.cache->radius,ss.cache->hardness,distances);
        BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                                     brush.curve_distance_falloff,distances,ss.cache->radius,factors);
        if(audit)coverage.curve+=coverage.count(factors,distances,ss.cache->radius);
        auto_mask::calc_vert_factors(depsgraph,object,ss.cache->automasking.get(),node,verts,factors);
        if(audit)coverage.automask+=coverage.count(factors,distances,ss.cache->radius);
        calc_brush_texture_factors(ss,brush,pos.eval,verts,factors);
        if(audit)coverage.texture+=coverage.count(factors,distances,ss.cache->radius);
      }else{
        calc_factors_common_mesh_indexed(depsgraph,brush,object,attributes,pos.eval,normals,node,factors,distances);
      }
      if (autonomous) {
        physics_v15::append_intent_samples(intent_samples,verts,pos.eval,factors,normals);
        return;
      }
      translations.resize(verts.size());
      // Reuse Clay's OWN plane projection, strength and final brush factors.
      // The physical executor receives finished goals and never reapplies
      // the displacement factor or replaces the clay plane with a normal.
      calc_closest_to_plane(test_plane,pos.eval,verts,translations);
      scale_translations(translations,bstrength);
      scale_translations(translations,factors);
      for (const int j : verts.index_range()) {
        const float3 value=translations[j];
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z)) continue;
        native.push_back({std::size_t(verts[j]),
                          {double(value.x),double(value.y),double(value.z)}});
      }
    });
    if(audit)coverage.report(ss.cache->xpbd_v16_clay_behavior);
    if (autonomous ?
        physics_v15::try_apply_intent(sd,object,node_mask,pos,
                                    sculpt_physics::NativeSculptBrushKind::Clay,
                                    intent_samples,area_no) :
        physics_v15::try_apply(sd,object,node_mask,pos,
                              sculpt_physics::NativeSculptBrushKind::Clay,native)) {
      return;
    }
  }
#endif

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object, true);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_faces(depsgraph,
                       sd,
                       brush,
                       test_plane,
                       bstrength,
                       attribute_data,
                       vert_normals,
                       nodes[i],
                       object,
                       tls,
                       position_data);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_grids(depsgraph, sd, object, brush, test_plane, bstrength, nodes[i], tls);
            bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_bmesh(depsgraph, sd, object, brush, test_plane, bstrength, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  apply_physical_material_finish(depsgraph, sd, brush, object, node_mask);
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

}  // namespace blender::ed::sculpt_paint::brushes
