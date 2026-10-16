/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include <cstdio>
#include "DNA_modifier_types.h"
#include "DEG_depsgraph_query.hh"
#include "multires_overlay_neutral_v01.hh"
#include "multires_draw_profile_v01.hh"
#include "draw_sculpt.hh"

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "draw_attributes.hh"
#include "draw_context_private.hh"
#include "draw_view.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_customdata.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_time.h"
#include "BLI_math_matrix.hh"

#include "bmesh_class.hh"

#include "DRW_pbvh.hh"
#include "DRW_render.hh"

namespace blender::draw {

float3 SculptBatch::debug_color()
{
  static float3 colors[9] = {
      {1.0f, 0.2f, 0.2f},
      {0.2f, 1.0f, 0.2f},
      {0.2f, 0.2f, 1.0f},
      {1.0f, 1.0f, 0.2f},
      {0.2f, 1.0f, 1.0f},
      {1.0f, 0.2f, 1.0f},
      {1.0f, 0.7f, 0.2f},
      {0.2f, 1.0f, 0.7f},
      {0.7f, 0.2f, 1.0f},
  };

  return colors[debug_index % 9];
}

/* MULTIRES_OVERLAY_NEUTRAL_V01: filter only the extra sculpt-overlay pass. */
static IndexMask multires_overlay_non_neutral_nodes(const Object &object,
                                                   const bke::pbvh::Tree &tree,
                                                   const IndexMask &visible,
                                                   const SculptBatchFeature features,
                                                   IndexMaskMemory &memory)
{
  if (tree.type() != bke::pbvh::Type::Grids) return visible;
  const SculptSession *ss = object.runtime->sculpt_session;
  if (!ss || !ss->subdiv_ccg || !ss->multires_modifier) return visible;
  const SubdivCCG &ccg = *ss->subdiv_ccg;
  const Object *original = DEG_get_original(&object);
  const Mesh *mesh = original ? id_cast<const Mesh *>(original->data) : nullptr;
  if (!mesh || ccg.grid_to_face_map.size() != ccg.grids_num) return visible;
  const bool mask = bool(features & SCULPT_BATCH_MASK);
  const bool face_set = bool(features & SCULPT_BATCH_FACE_SET);
  /* Match the data source used by fill_face_sets_grids. No scan of dense CCG data. */
  const VArraySpan face_sets = *mesh->attributes().lookup<int>(
      ".sculpt_face_set", bke::AttrDomain::Face);
  const bool high_res = ss->multires_modifier->sculpt_face_set_grids != nullptr;
  const Span<bke::pbvh::GridsNode> nodes = tree.nodes<bke::pbvh::GridsNode>();
  return IndexMask::from_predicate(visible, memory, [&](const int i) {
    return overlay_neutral_v01::needs_overlay(
        mask, !ccg.masks.is_empty(), BKE_pbvh_node_fully_unmasked_get(nodes[i]),
        face_set, high_res, !face_sets.is_empty(), mesh->face_sets_color_default,
        nodes[i].grids(), [&](const int grid) {
          if (grid < 0 || grid >= ccg.grid_to_face_map.size()) {
            return (mesh->face_sets_color_default ^ 1);
          }
          const int face = ccg.grid_to_face_map[grid];
          return face >= 0 && face < face_sets.size() ? face_sets[face] :
                                                      (mesh->face_sets_color_default ^ 1);
        });
  });
}

static Vector<SculptBatch> sculpt_batches_get_ex(const Object *ob,
                                                 const bool use_wire,
                                                 const Span<pbvh::AttributeRequest> attrs,
                                                 const SculptBatchFeature overlay_features = SCULPT_BATCH_DEFAULT)
{
  /* pbvh::Tree should always exist for non-empty meshes, created by depsgraph eval. */
  bke::pbvh::Tree *pbvh = ob->runtime->sculpt_session ?
                              const_cast<bke::pbvh::Tree *>(bke::object::pbvh_get(*ob)) :
                              nullptr;
  if (!pbvh) {
    return {};
  }

  /* TODO(Miguel Pozo): Don't use global context. */
  const DRWContext *drwctx = DRW_context_get();
  RegionView3D *rv3d = drwctx->rv3d;
  const bool navigating = rv3d && (rv3d->rflag & RV3D_NAVIGATING);

  Paint *paint = nullptr;
  if (drwctx->evil_C != nullptr) {
    paint = BKE_paint_get_active_from_context(drwctx->evil_C);
  }

  /* TODO: take into account partial redraw for clipping planes. */
  /* Frustum planes to show only visible pbvh::Tree nodes. */
  std::array<float4, 6> draw_frustum_planes = View::default_get().frustum_planes_get();
  /* Transform clipping planes to object space. Transforming a plane with a
   * 4x4 matrix is done by multiplying with the transpose inverse.
   * The inverse cancels out here since we transform by inverse(obmat). */
  float4x4 tmat = math::transpose(ob->object_to_world());
  for (int i : IndexRange(draw_frustum_planes.size())) {
    draw_frustum_planes[i] = tmat * draw_frustum_planes[i];
  }

  /* Fast mode to show low poly multires while navigating. */
  bool fast_mode = false;
  if (paint && (paint->flags & PAINT_FAST_NAVIGATE)) {
    fast_mode = navigating;
  }

  /* Update draw buffers only for visible nodes while painting.
   * But do update them otherwise so navigating stays smooth. */
  bool update_only_visible = rv3d && !(rv3d->rflag & RV3D_PAINTING);
  if (paint && (paint->flags & PAINT_SCULPT_DELAY_UPDATES)) {
    update_only_visible = true;
  }

  /* Sculpt Mesh V3 Draw memory probe.
   * Diagnostic only: identify large-mesh viewport stalls after the brush calculation. */
  const Mesh *sculpt_mesh_v3_draw_mesh = BKE_object_get_original_mesh(ob);
  const bool sculpt_mesh_v3_draw_enabled =
      pbvh->type() == bke::pbvh::Type::Mesh && sculpt_mesh_v3_draw_mesh &&
      sculpt_mesh_v3_draw_mesh->verts_num >= 1'000'000;
  const bool multires_draw_probe = pbvh->type() == bke::pbvh::Type::Grids &&
      multires_draw_profile_v01::path();
  const double sculpt_mesh_v3_draw_total_start = BLI_time_now_seconds();
  const double sculpt_mesh_v3_draw_normals_start = sculpt_mesh_v3_draw_total_start;  bke::pbvh::update_normals_from_eval(*const_cast<Object *>(ob), *pbvh);
  const double sculpt_mesh_v3_draw_normals_ms =
      (BLI_time_now_seconds() - sculpt_mesh_v3_draw_normals_start) * 1000.0;
  const double sculpt_mesh_v3_draw_search_start = BLI_time_now_seconds();

  pbvh::DrawCache &draw_data = pbvh::ensure_draw_data(pbvh->draw_data);

  IndexMaskMemory memory;
  IndexMask visible_nodes = bke::pbvh::search_nodes(
      *pbvh, memory, [&](const bke::pbvh::Node &node) {
        return !BKE_pbvh_node_fully_hidden_get(node) &&
               bke::pbvh::node_frustum_contain_aabb(node, draw_frustum_planes);
      });
  const double sculpt_mesh_v3_draw_search_ms =
      (BLI_time_now_seconds() - sculpt_mesh_v3_draw_search_start) * 1000.0;

  const bool filter_overlay = bool(overlay_features & SCULPT_BATCH_SKIP_NEUTRAL_OVERLAY) &&
                              pbvh->type() == bke::pbvh::Type::Grids;
  if (filter_overlay) {
    const int64_t before = visible_nodes.size();
    const double start = BLI_time_now_seconds();
    visible_nodes = multires_overlay_non_neutral_nodes(
        *ob, *pbvh, visible_nodes, overlay_features, memory);
    if (multires_draw_probe) {
      multires_draw_profile_v01::log(
          "[MULTIRES_OVERLAY_NEUTRAL_V01] visible=%lld kept=%lld skipped=%lld filter_ms=%.3f\n",
          (long long)before, (long long)visible_nodes.size(),
          (long long)(before - visible_nodes.size()),
          (BLI_time_now_seconds() - start) * 1000.0);
    }
    if (visible_nodes.is_empty()) return {};
  }

  /* V4.4c heavy Multires: avoid forcing VBO creation/upload for every PBVH leaf on
   * the first sculpt stroke. Large SubdivCCG surfaces keep draw preparation local
   * to the current frustum, matching Sculpt "Delay Viewport Updates" behavior.
   * Smaller meshes keep Blender's normal eager update policy. */
  constexpr int64_t heavy_multires_sample_threshold = 32'000'000;
  const SculptSession *sculpt_session = ob->runtime->sculpt_session;
  const bool heavy_multires_visible_only =
      pbvh->type() == bke::pbvh::Type::Grids && sculpt_session &&
      sculpt_session->subdiv_ccg &&
      sculpt_session->subdiv_ccg->positions.size() >= heavy_multires_sample_threshold;

  /* Sculpt Mesh V1: heavy base-mesh visible-only draw cache.
   *
   * For very large regular Mesh PBVH sculpting, avoid forcing VBO/batch
   * preparation for every PBVH leaf on each draw pass. Off-frustum dirty
   * nodes remain dirty in the DrawCache and are prepared when they become
   * visible later. This mirrors the proven V4.4c Multires strategy without
   * changing brush math, geometry, undo, normals or PBVH edit locality. */
  constexpr int64_t heavy_mesh_vertex_threshold = 15'000'000;
  const Mesh *source_mesh_for_threshold = BKE_object_get_original_mesh(ob);
  const bool heavy_mesh_visible_only =
      pbvh->type() == bke::pbvh::Type::Mesh && source_mesh_for_threshold &&
      source_mesh_for_threshold->verts_num >= heavy_mesh_vertex_threshold;

  const IndexMask nodes_to_update =
      (filter_overlay || update_only_visible || heavy_multires_visible_only || heavy_mesh_visible_only) ?
          visible_nodes :
          bke::pbvh::all_leaf_nodes(*pbvh, memory);

  const double sculpt_mesh_v3_draw_batches_start = BLI_time_now_seconds();
  Span<gpu::Batch *> batches;
  if (use_wire) {
    batches = draw_data.ensure_lines_batches(*ob, {{}, fast_mode}, nodes_to_update);
  }
  else {
    batches = draw_data.ensure_tris_batches(*ob, {attrs, fast_mode}, nodes_to_update);
  }

  const double sculpt_mesh_v3_draw_batches_ms =
      (BLI_time_now_seconds() - sculpt_mesh_v3_draw_batches_start) * 1000.0;
  const double sculpt_mesh_v3_draw_total_ms =
      (BLI_time_now_seconds() - sculpt_mesh_v3_draw_total_start) * 1000.0;

  if (sculpt_mesh_v3_draw_enabled && sculpt_mesh_v3_draw_total_ms >= 0.50) {
    std::fprintf(stderr,
                 "[SculptMeshV3Draw] visible=%lld update=%lld total=%.3f ms "
                 "normals=%.3f search=%.3f batches=%.3f\n",
                 static_cast<long long>(visible_nodes.size()),
                 static_cast<long long>(nodes_to_update.size()),
                 sculpt_mesh_v3_draw_total_ms,
                 sculpt_mesh_v3_draw_normals_ms,
                 sculpt_mesh_v3_draw_search_ms,
                 sculpt_mesh_v3_draw_batches_ms);
  }
  if (multires_draw_probe) {
    const SubdivCCG *ccg = sculpt_session ? sculpt_session->subdiv_ccg : nullptr;
    multires_draw_profile_v01::log(
        "[MULTIRES_DRAW] grids=%d area=%d wire=%d fast=%d painting=%d attrs=%lld "
        "visible=%lld requested=%lld normals_ms=%.3f search_ms=%.3f batches_ms=%.3f "
        "prepare_ms=%.3f\n",
        ccg ? ccg->grids_num : 0, ccg ? ccg->grid_area : 0,
        int(use_wire), int(fast_mode), int(rv3d && (rv3d->rflag & RV3D_PAINTING)),
        static_cast<long long>(attrs.size()), static_cast<long long>(visible_nodes.size()),
        static_cast<long long>(nodes_to_update.size()), sculpt_mesh_v3_draw_normals_ms,
        sculpt_mesh_v3_draw_search_ms, sculpt_mesh_v3_draw_batches_ms,
        sculpt_mesh_v3_draw_total_ms);
  }

  const Span<int> material_indices = draw_data.ensure_material_indices(*ob);

  const int max_material = std::max(0, BKE_object_material_count_eval(ob) - 1);
  Vector<SculptBatch> result_batches(visible_nodes.size());
  visible_nodes.foreach_index([&](const int i, const int pos) {
    result_batches[pos] = {};
    result_batches[pos].batch = batches[i];
    result_batches[pos].material_slot = material_indices.is_empty() ?
                                            0 :
                                            std::clamp(material_indices[i], 0, max_material);
    result_batches[pos].debug_index = pos;
  });

  return result_batches;
}

Vector<SculptBatch> sculpt_batches_get(const Object *ob, SculptBatchFeature features)
{
  Vector<pbvh::AttributeRequest, 16> attrs;

  attrs.append(pbvh::CustomRequest::Position);
  attrs.append(pbvh::CustomRequest::Normal);
  if (features & SCULPT_BATCH_MASK) {
    attrs.append(pbvh::CustomRequest::Mask);
  }
  if (features & SCULPT_BATCH_FACE_SET) {
    attrs.append(pbvh::CustomRequest::FaceSet);
  }

  const Mesh *mesh = BKE_object_get_original_mesh(ob);
  if (features & SCULPT_BATCH_VERTEX_COLOR) {
    if (const char *name = mesh->active_color_attribute) {
      attrs.append(pbvh::GenericRequest(name));
    }
  }

  if (features & SCULPT_BATCH_UV) {
    const StringRef uv_name = mesh->active_uv_map_name();
    if (!uv_name.is_empty()) {
      attrs.append(pbvh::GenericRequest(uv_name));
    }
  }

  return sculpt_batches_get_ex(ob, features & SCULPT_BATCH_WIREFRAME, attrs, features);
}

Vector<SculptBatch> sculpt_batches_per_material_get(const Object *ob,
                                                    Span<const GPUMaterial *> materials)
{
  BLI_assert(ob->type == OB_MESH);
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob);

  VectorSet<std::string> draw_attrs;
  DRW_MeshCDMask cd_needed;
  DRW_mesh_get_attributes(*ob, mesh, materials, &draw_attrs, &cd_needed);

  const bke::pbvh::Tree *tree = ob->runtime->sculpt_session ?
                                  bke::object::pbvh_get(*ob) :
                                  nullptr;
  if (tree && tree->type() == bke::pbvh::Type::Grids) {
    const Mesh *original_mesh = BKE_object_get_original_mesh(ob);
    if (original_mesh && original_mesh != &mesh) {
      /* A first paint stroke can add the color marker to the original mesh
       * while its evaluated copy is still stale. Only supplement the active
       * Polypaint request, and only when a material actually requests it. */
      const StringRef active_color = original_mesh->active_color_attribute;
      if (!active_color.is_empty()) {
        VectorSet<std::string> original_attrs;
        DRW_MeshCDMask original_cd_needed;
        DRW_mesh_get_attributes(
            *ob, *original_mesh, materials, &original_attrs, &original_cd_needed);
        if (original_attrs.contains(std::string(active_color))) {
          draw_attrs.add(std::string(active_color));
        }
      }
    }
  }

  Vector<pbvh::AttributeRequest, 16> attrs;

  attrs.append(pbvh::CustomRequest::Position);
  attrs.append(pbvh::CustomRequest::Normal);

  for (const StringRef name : draw_attrs) {
    attrs.append(pbvh::GenericRequest(name));
  }

  for (const StringRef name : cd_needed.uv) {
    attrs.append(pbvh::GenericRequest(name));
  }

  return sculpt_batches_get_ex(ob, false, attrs);
}

}  // namespace blender::draw
