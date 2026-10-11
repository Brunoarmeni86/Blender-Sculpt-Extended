/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_kelvinlet.h"
#include "elastic_grab_exact.hh"
#include "elastic_grab_level_one.hh"
#include <cstdlib>
#include <algorithm>
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace elastic_deform_cc {

struct LocalData {
  Vector<float> factors;
  Vector<float3> translations;
};

BLI_NOINLINE static void calc_translations(const Brush &brush,
                                           const StrokeCache &cache,
                                           const KelvinletParams &kelvinet_params,
                                           const float3 &location,
                                           const float3 &offset,
                                           const Span<float3> positions,
                                           const Span<float> factors,
                                           const bool cache_weights,
                                           const MutableSpan<float3> translations)
{
  PRF_scope(ProfileCategory::Editor);
  /* SCULPT_ELASTIC_GRAB_EXACT_V1: opt-in, no persistent vertex cache. */
  /* SCULPT_ELASTIC_MODES_UI_V1: the saved brush setting owns mode selection. */
  const bool use_exact = (brush.flag2 & (BRUSH_SCULPT_ELASTIC_EXACT |
                                       BRUSH_SCULPT_ELASTIC_ACCELERATED)) != 0;
  const int iterations = brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_GRAB ? 1 :
                         brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_GRAB_BISCALE ? 2 :
                         brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_GRAB_TRISCALE ? 3 : 0;
  if (use_exact && iterations != 0) {
    const PreparedElasticGrab prepared(kelvinet_params, iterations);
    bool all_masked = true;
    for (const float factor : factors) {
      if (factor != 0.0f) { all_masked = false; break; }
    }
    if (all_masked) {
      translations.fill(float3(0.0f));
      return;
    }
    std::shared_ptr<ElasticGrabWeightCache::Entry> entry;
    if (cache_weights && cache.elastic_grab_weight_cache) {
      entry = cache.elastic_grab_weight_cache->acquire(
          positions.data(), size_t(positions.size()), prepared, location);
    }
    /* Node/symmetry tasks normally have separate entries. The lock also protects
     * the lazy fill if two tasks ever share an undo position span. */
    std::unique_lock<std::mutex> entry_lock;
    if (entry) { entry_lock = std::unique_lock<std::mutex>(entry->mutex); }
    for (const int i : positions.index_range()) {
      if (factors[i] == 0.0f) {
        translations[i] = float3(0.0f);
      }
      else if (entry) {
        float &fade = entry->weights[i];
        if (std::isnan(fade)) { fade = prepared.fade_at(positions[i], location); }
        translations[i] = offset * fade;
      }
      else {
        prepared.translation(translations[i], positions[i], location, offset);
      }
    }
    scale_translations(translations, cache.bstrength * 20.0f);
    return;
  }
  switch (eBrushElasticDeformType(brush.elastic_deform_type)) {
    case BRUSH_ELASTIC_DEFORM_GRAB: {
      for (const int i : positions.index_range()) {
        BKE_kelvinlet_grab(translations[i], &kelvinet_params, positions[i], location, offset);
      }
      scale_translations(translations, cache.bstrength * 20.0f);
      break;
    }
    case BRUSH_ELASTIC_DEFORM_GRAB_BISCALE: {
      for (const int i : positions.index_range()) {
        BKE_kelvinlet_grab_biscale(
            translations[i], &kelvinet_params, positions[i], location, offset);
      }
      scale_translations(translations, cache.bstrength * 20.0f);
      break;
    }
    case BRUSH_ELASTIC_DEFORM_GRAB_TRISCALE: {
      for (const int i : positions.index_range()) {
        BKE_kelvinlet_grab_triscale(
            translations[i], &kelvinet_params, positions[i], location, offset);
      }
      scale_translations(translations, cache.bstrength * 20.0f);
      break;
    }
    case BRUSH_ELASTIC_DEFORM_SCALE: {
      for (const int i : positions.index_range()) {
        BKE_kelvinlet_scale(
            translations[i], &kelvinet_params, positions[i], location, cache.sculpt_normal_symm);
      }
      break;
    }
    case BRUSH_ELASTIC_DEFORM_TWIST: {
      for (const int i : positions.index_range()) {
        BKE_kelvinlet_twist(
            translations[i], &kelvinet_params, positions[i], location, cache.sculpt_normal_symm);
      }
      break;
    }
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const KelvinletParams &kelvinet_params,
                       const float3 &offset,
                       const MeshAttributeData &attribute_data,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);
  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, orig_data.positions, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_translations(brush,
                    cache,
                    kelvinet_params,
                    cache.location_symm,
                    offset,
                    orig_data.positions,
                    factors,
                    true,
                    translations);

  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

/* SCULPT_ELASTIC_ACCELERATED_V1: opt-in approximation on the level-1 grid lattice. */
static bool calc_level_one_translations(const Brush &brush,
                                        const StrokeCache &cache,
                                        const KelvinletParams &params,
                                        const float3 &offset,
                                        const int side,
                                        const Span<float3> positions,
                                        const Span<float> factors,
                                        const MutableSpan<float3> translations)
{
  if ((brush.flag2 & BRUSH_SCULPT_ELASTIC_ACCELERATED) == 0) { return false; }
  const int iterations = brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_GRAB ? 1 :
                         brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_GRAB_BISCALE ? 2 :
                         brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_GRAB_TRISCALE ? 3 : 0;
  if (iterations == 0 || side < 5) { return false; }
  const PreparedElasticGrab prepared(params, iterations);
  const float tolerance = std::fabs(prepared.fade_at(cache.location_symm, cache.location_symm)) * 0.005f;
  const int area = side * side;
  /* SCULPT_ELASTIC_ACCELERATED_V2: same immutable undo-position key as Exact,
   * but an entirely separate store. Allocation failure keeps Accelerated V1. */
  std::shared_ptr<ElasticGrabWeightCache::Entry> entry;
  if (cache.elastic_grab_accelerated_cache) {
    entry = cache.elastic_grab_accelerated_cache->acquire(
        positions.data(), size_t(positions.size()), prepared, cache.location_symm);
  }
  std::unique_lock<std::mutex> entry_lock;
  if (entry) { entry_lock = std::unique_lock<std::mutex>(entry->mutex); }
  for (int begin = 0; begin < positions.size(); begin += area) {
    bool complete = bool(entry);
    if (entry) {
      for (int i=0;i<area;i++) {
        if (factors[begin+i] != 0.0f && std::isnan(entry->weights[begin+i])) {
          complete=false;
          break;
        }
      }
    }
    if (complete) {
      for (int i=0;i<area;i++) {
        const int index=begin+i;
        translations[index] = factors[index] == 0.0f ? float3(0.0f) :
                                                       offset * entry->weights[index];
      }
      continue;
    }
    bool any = false;
    for (int i=0;i<area;i++) { if (factors[begin+i] != 0.0f) { any=true; break; } }
    if (!any) {
      for (int i=0;i<area;i++) { translations[begin+i]=float3(0.0f); }
      continue;
    }
    const auto evaluate = [&](int x,int y) {
      return prepared.fade_at(positions[begin+y*side+x], cache.location_symm);
    };
    ElasticLevelOneField field;
    const bool accepted = field.prepare(side, evaluate, tolerance);
    for (int y=0;y<side;y++) {
      for (int x=0;x<side;x++) {
        const int i=begin+y*side+x;
        if (factors[i]==0.0f) { translations[i]=float3(0.0f); continue; }
        float fade;
        if (entry && !std::isnan(entry->weights[i])) { fade=entry->weights[i]; }
        else {
          fade = accepted && !ElasticLevelOneField::boundary(x,y,side) ?
                     field.interpolate(x,y,side) : evaluate(x,y);
          if (entry) { entry->weights[i]=fade; }
        }
        translations[i]=offset*fade;
      }
    }
  }
  scale_translations(translations, cache.bstrength * 20.0f);
  return true;
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const KelvinletParams &kelvinet_params,
                       const float3 &offset,
                       bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  const OrigPositionData orig_data = orig_position_data_get_grids(object, node);
  const Span<int> grids = node.grids();
  const int grid_verts_num = grids.size() * key.grid_area;

  tls.factors.resize(grid_verts_num);
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, orig_data.positions, factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  tls.translations.resize(grid_verts_num);
  const MutableSpan<float3> translations = tls.translations;
  if (!calc_level_one_translations(brush, cache, kelvinet_params, offset,
                                   key.grid_size, orig_data.positions, factors, translations)) {
  calc_translations(brush,
                    cache,
                    kelvinet_params,
                    cache.location_symm,
                    offset,
                    orig_data.positions,
                    factors,
                    true,
                    translations);
  }

  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, orig_data.positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const KelvinletParams &kelvinet_params,
                       const float3 &offset,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  Array<float3> orig_positions(verts.size());
  Array<float3> orig_normals(verts.size());
  orig_position_data_gather_bmesh(*ss.bm_log, verts, orig_positions, orig_normals);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, orig_positions, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  calc_translations(
      brush, cache, kelvinet_params, cache.location_symm, offset, orig_positions, factors, false, translations);

  scale_translations(translations, factors);

  clip_and_lock_translations(sd, ss, orig_positions, translations);
  apply_translations(translations, verts);
}

}  // namespace elastic_deform_cc

void do_elastic_deform_brush(const Depsgraph &depsgraph,
                             const Sculpt &sd,
                             Object &object,
                             const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float strength = ss.cache->bstrength;

  /* SCULPT_ELASTIC_GRAB_EXACT_V2: allocate once on the invoking thread,
   * before parallel node tasks. Cache admission failure preserves the V1 path. */
  const bool exact_mode = (brush.flag2 & (BRUSH_SCULPT_ELASTIC_EXACT |
                                        BRUSH_SCULPT_ELASTIC_ACCELERATED)) != 0;
  const char *cache_option = std::getenv("BLENDER_SCULPT_ELASTIC_CACHE");
  const bool accelerated = (brush.flag2 & BRUSH_SCULPT_ELASTIC_ACCELERATED) != 0 &&
                           pbvh.type() == bke::pbvh::Type::Grids;
  /* Keep only the active mode's cache, with the existing total admission budget. */
  auto &weight_cache = accelerated ? ss.cache->elastic_grab_accelerated_cache :
                                    ss.cache->elastic_grab_weight_cache;
  if (accelerated) { ss.cache->elastic_grab_weight_cache.reset(); }
  else { ss.cache->elastic_grab_accelerated_cache.reset(); }
  const bool enable_cache = exact_mode &&
                            (!cache_option || cache_option[0] != '0') &&
                            pbvh.type() != bke::pbvh::Type::BMesh &&
                            ELEM(brush.elastic_deform_type, BRUSH_ELASTIC_DEFORM_GRAB,
                                 BRUSH_ELASTIC_DEFORM_GRAB_BISCALE, BRUSH_ELASTIC_DEFORM_GRAB_TRISCALE);
  if (enable_cache && !weight_cache) {
    long megabytes = 128;
    if (const char *setting = std::getenv("BLENDER_SCULPT_ELASTIC_CACHE_MB")) {
      char *end = nullptr;
      const long parsed = std::strtol(setting, &end, 10);
      if (end != setting && *end == '\0') { megabytes = std::clamp(parsed, 0L, 512L); }
    }
    if (megabytes > 0) {
      try {
        weight_cache = std::make_shared<ElasticGrabWeightCache>(
            size_t(megabytes) * 1024 * 1024);
      }
      catch (const std::bad_alloc &) { /* Keep the Exact V1 path. */ }
    }
  }
  if (!enable_cache) { weight_cache.reset(); }

  const float3 grab_delta = grab_delta_get(brush, *ss.cache);

  float dir;
  if (ss.cache->mouse[0] > ss.cache->initial_mouse[0]) {
    dir = 1.0f;
  }
  else {
    dir = -1.0f;
  }

  if (brush.elastic_deform_type == BRUSH_ELASTIC_DEFORM_TWIST) {
    int symm = ss.cache->mirror_symmetry_pass;
    if (ELEM(symm, 1, 2, 4, 7)) {
      dir = -dir;
    }
  }

  KelvinletParams params;
  float force = math::length(grab_delta) * dir * strength;
  BKE_kelvinlet_init_params(
      &params, ss.cache->radius, force, 1.0f, brush.elastic_deform_volume_preservation);

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_faces(depsgraph,
                       sd,
                       brush,
                       params,
                       grab_delta,
                       attribute_data,
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
            calc_grids(depsgraph, sd, object, brush, params, grab_delta, nodes[i], tls);
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
            calc_bmesh(depsgraph, sd, object, brush, params, grab_delta, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
    } break;
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

}  // namespace blender::ed::sculpt_paint::brushes
