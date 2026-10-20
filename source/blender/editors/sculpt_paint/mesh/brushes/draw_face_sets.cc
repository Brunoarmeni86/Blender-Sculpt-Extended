/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"
#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"

#include "DNA_brush_types.h"
#include "DNA_modifier_types.h"

#include <cstdio>
#include <algorithm>
#include "BKE_modifier.hh"
#include "MEM_guardedalloc.h"
#include <variant>

#include "BKE_multires_sculpt_layers.hh"
#include "BKE_sculpt_facesets_dense_probe_v06d.hh"
#include "BKE_sculpt_facesets_rc1.hh"
#include "BKE_ccg.hh"
#include "BKE_subdiv.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include <cstdlib>
#include "BLI_math_base.hh"
#include "BLI_task.hh"

#include "PRF_profile.hh"

#include "editors/sculpt_paint/mesh/sculpt_face_set.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"
#include "editors/sculpt_paint/mesh/sculpt_undo.hh"

#include "ED_sculpt.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {
inline namespace draw_face_sets_cc {

/* MULTIRES_FACESETS_DRAW_V01: callers initialize/validate the master serially,
 * before Undo workers or paint workers can touch its descriptors. */
static bool rebuild_brush_v05_enabled()
{
  const char *v=std::getenv("BLENDER_FACESETS_BRUSH_V05");
  return v && v[0]=='1' && v[1]=='\0';
}
static bool use_high_resolution_face_sets(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  return (rebuild_brush_v05_enabled() || bke::multires_layers::faceset_high_resolution_enabled()) &&
         bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids &&
         ss.multires_modifier && ss.subdiv_ccg;
}

static bool prepare_high_resolution_face_sets(Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  MultiresModifierData &mmd = *ss.multires_modifier;
  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
  if (mmd.totlvl < 1 || mmd.totlvl > 13 || key.grid_size < 2 ||
      ss.subdiv_ccg->grids_num != mesh.corners_num ||
      !bke::multires_layers::faceset_ensure_from_mesh(mmd, mesh, mmd.totlvl) ||
      !bke::multires_layers::faceset_master_valid(mmd, mesh.corners_num))
  {
    return false;
  }
  const int side = bke::subdiv::grid_size_from_level(mmd.sculpt_face_set_level);
  return key.grid_size <= side && (side - 1) % (key.grid_size - 1) == 0;
}

static int pick_high_resolution_face_set(const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const int fallback = face_set::active_face_set_get(object);
  const auto active_vert = ss.active_vert();
  const int *active = std::get_if<int>(&active_vert);
  if (!active || *active < 0) {
    return fallback;
  }
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
  if (int64_t(*active) >= int64_t(ss.subdiv_ccg->grids_num) * key.grid_area) {
    return fallback;
  }
  const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, *active);
  return bke::multires_layers::faceset_sample(*ss.multires_modifier,
                                             coord.grid_index,
                                             float(coord.x) / float(key.grid_size - 1),
                                             float(coord.y) / float(key.grid_size - 1),
                                             fallback);
}


struct MeshLocalData {
  Vector<float3> positions;
  Vector<float3> normals;
  Vector<float> factors;
  Vector<float> distances;
};

static void calc_face_normals(const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              const Span<float3> vert_positions,
                              const Span<int> face_indices,
                              const MutableSpan<float3> normals)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(face_indices.size() == normals.size());

  for (const int i : face_indices.index_range()) {
    normals[i] = bke::mesh::face_normal_calc(vert_positions,
                                             corner_verts.slice(faces[face_indices[i]]));
  }
}

BLI_NOINLINE static void apply_face_set(const int face_set_id,
                                        const Span<int> face_indices,
                                        const Span<float> factors,
                                        const MutableSpan<int> face_sets)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(face_indices.size() == factors.size());

  for (const int i : face_indices.index_range()) {
    if (factors[i] > face_set::FACE_SET_MIN_FADE) {
      face_sets[face_indices[i]] = face_set_id;
    }
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       const int face_set_id,
                       Span<float3> positions_eval,
                       const bke::pbvh::MeshNode &node,
                       const Span<int> face_indices,
                       MeshLocalData &tls,
                       const MutableSpan<int> face_sets)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  tls.positions.resize(face_indices.size());
  const MutableSpan<float3> face_centers = tls.positions;
  face_set::calc_face_centers(faces, corner_verts, positions_eval, face_indices, face_centers);

  tls.normals.resize(face_indices.size());
  const MutableSpan<float3> face_normals = tls.normals;
  calc_face_normals(faces, corner_verts, positions_eval, face_indices, face_normals);

  tls.factors.resize(face_indices.size());
  const MutableSpan<float> factors = tls.factors;

  face_set::fill_factor_from_hide_and_mask(mesh, face_indices, factors);

  filter_region_clip_factors(ss, face_centers, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, face_normals, factors);
  }

  tls.distances.resize(face_indices.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(ss, face_centers, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  if (cache.automasking) {
    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();
    auto_mask::calc_face_factors(
        depsgraph, object, faces, corner_verts, *cache.automasking, node, face_indices, factors);
  }

  calc_brush_texture_factors(ss, brush, face_centers, factors);
  scale_factors(factors, strength);

  apply_face_set(face_set_id, face_indices, factors, face_sets);
  if (bke::facesets_rc1::enabled()) {
    MultiresModifierData *mmd=bke::facesets_rc1::modifier(object);
    if (mmd && mmd->sculptlvl==0) {
      for (const int i:face_indices.index_range()) {
        if (!(factors[i]>face_set::FACE_SET_MIN_FADE)) { continue; }
        for (const int corner:faces[face_indices[i]]) {
          auto &grid=mmd->sculpt_face_set_grids[corner];
          MEM_SAFE_DELETE(grid.ids);
          grid.totid=0;grid.level=mmd->sculpt_face_set_level;grid.uniform_id=face_set_id;
        }
      }
    }
  }
}

static void do_draw_face_sets_brush_mesh(const Depsgraph &depsgraph,
                                         Object &object,
                                         const Brush &brush,
                                         const IndexMask &node_mask)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<float3> positions_eval = bke::pbvh::vert_positions_eval(depsgraph, object);

  if (bke::facesets_rc1::enabled()) {
    MultiresModifierData *mmd=bke::facesets_rc1::modifier(object);
    if (mmd && mmd->sculptlvl==0) {
      const Mesh &mesh=*id_cast<Mesh *>(object.data);
      Vector<int> corners;
      const Span<bke::pbvh::MeshNode> mesh_nodes=pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index([&](const int node) {
        for (const int face:mesh_nodes[node].faces()) {
          for (const int corner:mesh.faces()[face]) { corners.append(corner); }
        }
      });
      if (corners.is_empty()) { return; }
      std::sort(corners.begin(),corners.end());
      const int unique_count=int(std::unique(corners.begin(),corners.end())-corners.begin());
      corners.resize(unique_count);
      if (corners.is_empty()) { return; }
      if (const char *error=undo::capture_facesets_l0_master(*mmd,mesh.corners_num,corners.as_span())) {
        std::fprintf(stderr,"[FACESETS_RC1] %s; no L0 write\n",error);return;
      }
    }
  }
  undo::push_nodes(depsgraph, object, node_mask, undo::Type::FaceSet);

  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(
      *id_cast<Mesh *>(object.data));

  threading::EnumerableThreadSpecific<MeshLocalData> all_tls;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        MeshLocalData &tls = all_tls.local();
        const Span<int> face_indices = nodes[i].faces();
        calc_faces(depsgraph,
                   object,
                   brush,
                   ss.cache->bstrength,
                   ss.cache->paint_face_set,
                   positions_eval,
                   nodes[i],
                   face_indices,
                   tls,
                   face_sets.span);
      },
      exec_mode::grain_size(1));
  pbvh.tag_face_sets_changed(node_mask);
  face_sets.finish();
}

struct GridLocalData {
  Vector<int> face_indices;
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
};

static void calc_grids(const Depsgraph &depsgraph,
                       Object &object,
                       const Brush &brush,
                       const float strength,
                       const int face_set_id,
                       const bke::pbvh::GridsNode &node,
                       GridLocalData &tls,
                       const MutableSpan<int> face_sets)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  ed::sculpt_paint::fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
  }

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);
  scale_factors(factors, strength);

  if (use_high_resolution_face_sets(object)) {
    const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
    for (const int i : grids.index_range()) {
      bke::multires_layers::faceset_paint_grid(
          *ss.multires_modifier, grids[i], key.grid_size,
          factors.slice(bke::ccg::grid_range(key, i)), face_set::FACE_SET_MIN_FADE, face_set_id);
    }
    return;
  }

  tls.face_indices.resize(positions.size());
  MutableSpan<int> face_indices = tls.face_indices;
  face_set::calc_face_indices_grids(subdiv_ccg, grids, face_indices);
  apply_face_set(face_set_id, face_indices, factors, face_sets);
}

static void do_draw_face_sets_brush_grids(const Depsgraph &depsgraph,
                                          Object &object,
                                          const Brush &brush,
                                          const IndexMask &node_mask)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  undo::push_nodes(depsgraph, object, node_mask, undo::Type::FaceSet);

  /* The HR master is independent of the coarse Face Set attribute. Allocating
   * that attribute during an HR stroke can invalidate the evaluated mesh and
   * make a freshly painted grid disappear on the next level evaluation. */
  const bool high_resolution = use_high_resolution_face_sets(object);
  const auto paint_nodes = [&](const MutableSpan<int> base_face_sets) {
    threading::EnumerableThreadSpecific<GridLocalData> all_tls;
    MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
    node_mask.foreach_index(
        [&](const int i) {
          GridLocalData &tls = all_tls.local();
          calc_grids(depsgraph,
                     object,
                     brush,
                     ss.cache->bstrength,
                     ss.cache->paint_face_set,
                     nodes[i],
                     tls,
                     base_face_sets);
        },
        exec_mode::grain_size(1));
  };
  if (high_resolution) {
    paint_nodes(MutableSpan<int>());
  }
  else {
    bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(
        *id_cast<Mesh *>(object.data));
    paint_nodes(face_sets.span);
    face_sets.finish();
  }
  pbvh.tag_face_sets_changed(node_mask);
}
struct BMeshLocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
};

BLI_NOINLINE static void apply_face_set(const int face_set_id,
                                        const Set<BMFace *, 0> &faces,
                                        const MutableSpan<float> factors,
                                        const int cd_offset)
{
  int i = 0;
  for (BMFace *face : faces) {
    if (factors[i] > face_set::FACE_SET_MIN_FADE) {
      BM_ELEM_CD_SET_INT(face, cd_offset, face_set_id);
    }
    i++;
  }
}

static void calc_bmesh(Object &object,
                       const Brush &brush,
                       const float strength,
                       const int face_set_id,
                       bke::pbvh::BMeshNode &node,
                       BMeshLocalData &tls,
                       const int cd_offset)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(&node);
  tls.positions.resize(faces.size());
  const MutableSpan<float3> positions = tls.positions;
  face_set::calc_face_centers(faces, positions);

  tls.factors.resize(faces.size());
  const MutableSpan<float> factors = tls.factors;
  face_set::fill_factor_from_hide_and_mask(*ss.bm, faces, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, faces, factors);
  }

  tls.distances.resize(faces.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(ss, positions, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);
  scale_factors(factors, strength);

  apply_face_set(face_set_id, faces, factors, cd_offset);
}

static void do_draw_face_sets_brush_bmesh(const Depsgraph &depsgraph,
                                          Object &object,
                                          const Brush &brush,
                                          const IndexMask &node_mask)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  undo::push_nodes(depsgraph, object, node_mask, undo::Type::FaceSet);

  const int cd_offset = face_set::ensure_face_sets_bmesh(object);

  threading::EnumerableThreadSpecific<BMeshLocalData> all_tls;
  MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        BMeshLocalData &tls = all_tls.local();
        calc_bmesh(object,
                   brush,
                   ss.cache->bstrength,
                   ss.cache->paint_face_set,
                   nodes[i],
                   tls,
                   cd_offset);
      },
      exec_mode::grain_size(1));
  pbvh.tag_face_sets_changed(node_mask);
}

}  // namespace draw_face_sets_cc

void do_draw_face_sets_brush(const Depsgraph &depsgraph,
                             const Sculpt &sd,
                             Object &object,
                             const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  MultiresModifierData *rc_mmd=bke::facesets_rc1::enabled()?bke::facesets_rc1::modifier(object):nullptr;
  if (bke::facesets_rc1::enabled()) {
    const Mesh &mesh=*id_cast<Mesh *>(object.data);
    if (rc_mmd) {
      if (!bke::facesets_rc1::eligible(object,mesh,*rc_mmd) || mesh.id.us!=1 ||
          bke::object::pbvh_get(object)->type()==bke::pbvh::Type::BMesh ||
          !ELEM(brush.stroke_method,BRUSH_STROKE_DOTS,BRUSH_STROKE_SPACE)) {
        std::fprintf(stderr,"[FACESETS_RC1] unsupported mesh; no Face Set write\n");return;
      }
      auto base_ids=face_set::ensure_face_sets_mesh(*id_cast<Mesh *>(object.data));
      base_ids.finish();
      if (!bke::multires_layers::faceset_ensure_from_mesh(*rc_mmd,mesh,rc_mmd->totlvl) ||
          !bke::facesets_rc1::master_valid(object,mesh,*rc_mmd)) {
        std::fprintf(stderr,"[FACESETS_RC1] invalid master; no Face Set write\n");return;
      }
    }
    if (!rc_mmd && BKE_modifiers_findby_type(&object,eModifierType_Multires)) {
      std::fprintf(stderr,"[FACESETS_RC1] requires one active Multires modifier; no write\n");return;
    }
  }
  const bool high_resolution = use_high_resolution_face_sets(object);
  const bool rc_l0=rc_mmd && rc_mmd->sculptlvl==0 &&
      bke::object::pbvh_get(object)->type()==bke::pbvh::Type::Mesh;
  if (rebuild_brush_v05_enabled() && !bke::facesets_rc1::enabled()) {
    const SculptSession &ss=*object.runtime->sculpt_session;
    const Mesh &mesh=*id_cast<const Mesh *>(object.data);
    /* First brush probe deliberately requires the exact small, prepared fixture.
     * Never silently fall back to coarse base-face painting at L0. */
    const bool old_fixture = high_resolution && mesh.verts_num==8 && mesh.faces_num==6 &&
        mesh.corners_num==24 && STREQ(object.id.name+2,"FaceSets_Rebuild_L0_Test") &&
        ss.multires_modifier->totlvl==3 && ss.multires_modifier->sculpt_face_set_level==3 &&
        bke::multires_layers::faceset_master_valid(*ss.multires_modifier,24);
    const bool dense_fixture = high_resolution &&
        bke::facesets_dense_v06d::master_valid(object,mesh,*ss.multires_modifier);
    if (!old_fixture && !dense_fixture) {
      std::fprintf(stderr,"[FACESETS_BRUSH_V05] requires generated V05/V06d fixture, Grids and prepared L3 master; no brush write\n");
      return;
    }
  }
  if (object.runtime->sculpt_session->cache->paint_face_set == face_set_none_id) {
    if (high_resolution && !prepare_high_resolution_face_sets(object)) {
      std::fprintf(stderr, "[FACESETS_HR_V01] invalid master/layout; no Face Set painted\n");
      return;
    }
    if (object.runtime->sculpt_session->cache->toggle_settings.invert) {
      object.runtime->sculpt_session->cache->paint_face_set = high_resolution ?
          pick_high_resolution_face_set(object) : face_set::active_face_set_get(object);
    }
    else {
      object.runtime->sculpt_session->cache->paint_face_set = (high_resolution || rc_l0) ?
          bke::multires_layers::faceset_next_available_id(
              *(rc_l0?rc_mmd:object.runtime->sculpt_session->multires_modifier), 2) :
          face_set::find_next_available_id(object);
    }
    if (object.runtime->sculpt_session->cache->paint_face_set <= 0) {
      return;
    }
    if (rebuild_brush_v05_enabled() && !rc_l0 && object.runtime->sculpt_session->multires_modifier) {
      std::printf("[FACESETS_BRUSH_V05] native stroke id=%d level=%d master=%d\n",
          object.runtime->sculpt_session->cache->paint_face_set,
          int(object.runtime->sculpt_session->multires_modifier->sculptlvl),
          object.runtime->sculpt_session->multires_modifier->sculpt_face_set_level);
      std::fflush(stdout);
    }
  }

  switch (bke::object::pbvh_get(object)->type()) {
    case bke::pbvh::Type::Mesh:
      do_draw_face_sets_brush_mesh(depsgraph, object, brush, node_mask);
      break;
    case bke::pbvh::Type::Grids:
      do_draw_face_sets_brush_grids(depsgraph, object, brush, node_mask);
      break;
    case bke::pbvh::Type::BMesh:
      do_draw_face_sets_brush_bmesh(depsgraph, object, brush, node_mask);
      break;
  }
}
}  // namespace blender::ed::sculpt_paint::brushes
