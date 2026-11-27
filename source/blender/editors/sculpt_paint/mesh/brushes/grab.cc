/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <queue>
#if defined(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL) && !defined(BSE_VOLUMETRIC_ONLY)
#  include <algorithm>
#  include <cmath>
#  include <cstdio>
#  include <stdexcept>
#  include <utility>
#  include <cstdlib>
#  include <vector>
#  include "physics_core.hh"
#  ifdef WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14
#    include <unordered_map>
#    include <unordered_set>
#    include "native_grab_executor_adapter.hh"
#  endif
#  include "brush_solver_backend.hh"
#endif

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#if defined(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL) && !defined(BSE_VOLUMETRIC_ONLY)
#  include "BKE_multires_sculpt_layers.hh"
#endif
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

namespace blender {

namespace ed::sculpt_paint::brushes {
inline namespace grab_cc {

struct LocalData {
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

BLI_NOINLINE static void calc_silhouette_factors(const StrokeCache &cache,
                                                 const float3 &offset,
                                                 const Span<float3> normals,
                                                 const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(normals.size() == factors.size());

  const float sign = math::sign(math::dot(cache.initial_normal_symm, cache.grab_delta_symm));
  const float3 test_dir = math::normalize(offset) * sign;
  for (const int i : factors.index_range()) {
    factors[i] *= std::max(math::dot(test_dir, normals[i]), 0.0f);
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
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

  calc_factors_common_from_orig_data_mesh(depsgraph,
                                          brush,
                                          object,
                                          attribute_data,
                                          orig_data.positions,
                                          orig_data.normals,
                                          node,
                                          tls.factors,
                                          tls.distances);

  if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
    calc_silhouette_factors(cache, offset, orig_data.normals, tls.factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
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

  calc_factors_common_from_orig_data_grids(depsgraph,
                                           brush,
                                           object,
                                           orig_data.positions,
                                           orig_data.normals,
                                           node,
                                           tls.factors,
                                           tls.distances);

  if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
    calc_silhouette_factors(cache, offset, orig_data.normals, tls.factors);
  }

  tls.translations.resize(grid_verts_num);
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, orig_data.positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
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

  calc_factors_common_from_orig_data_bmesh(
      depsgraph, brush, object, orig_positions, orig_normals, node, tls.factors, tls.distances);

  if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
    calc_silhouette_factors(cache, offset, orig_normals, tls.factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, orig_positions, translations);
  apply_translations(translations, verts);
}

#if defined(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL) && !defined(BSE_VOLUMETRIC_ONLY)
/* V13: deliberately conservative persistent native-scene integration.
 * Runtime OFF by default, normal PBVH Mesh only. The physics proxy is
 * constructed once per Blender stroke and released with StrokeCache.
 * The active set is restricted to vertices in the existing sculpt node mask
 * to avoid writing unrecorded vertices outside Blender's native Undo region.
 * This is NOT yet the final multi-island/Multires integration. */
static bool try_xpbd_grab_preview(const Depsgraph &depsgraph,
                                  const Sculpt &sd,
                                  Object &object,
                                  const IndexMask &node_mask,
                                  const Brush &brush,
                                  const float3 &grab_delta,
                                  bke::pbvh::Tree &pbvh)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  const auto reject = [&](const char *reason) {
    if (!cache.xpbd_v12_rejection_logged) {
      std::fprintf(stderr, "[V12 XPBD Grab] standard fallback: %s\n", reason);
      cache.xpbd_v12_rejection_logged = true;
    }
    return false;
  };
  const char *enabled = std::getenv("BLENDER_SCULPT_XPBD_GRAB_V12");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return false;
  }
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return reject("PBVH is not an ordinary mesh");
  }
  // Do not bypass Sculpt Extended's Multires base-layer recording/CCG logic.
  // This first gate is only for an unmodified ordinary mesh object.
  if (bke::multires_layers::base_modifier(object) != nullptr) {
    return reject("Multires object is not supported by the V12 mesh adapter");
  }
  // PAINT_SYMMETRY_FEATHER is ON by default even with no symmetry axes.
  // Reject only actual mirror/tile axes, not the harmless feather option.
  const ePaintSymmetryFlags tile_symmetry = ePaintSymmetryFlags(
      PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z);
  if (cache.xpbd_v12_failed) return reject("solver failed earlier in this stroke");
  if (ss.deform_modifiers_active) return reject("deform modifier is active");
  if (ss.shapekey_active) return reject("Shape Key is active");
  if ((sd.paint.symmetry_flags & tile_symmetry) != PAINT_SYMM_NONE) {
    return reject("tile symmetry is active");
  }
  if (cache.tile_pass != 0) return reject("non-primary tile pass");
  if (cache.radial_symmetry_pass != 0) return reject("radial symmetry is active");
  const int mirror_pass = int(cache.mirror_symmetry_pass);
  if (mirror_pass < 0 || mirror_pass >= 8) {
    return reject("invalid mirror symmetry pass");
  }

  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  // V13 persistent proxy: no fixed vertex ceiling. Allocation or solver failures
  // remain transactional and fall back to Blender's standard Grab path.
  if (mesh.verts_num == 0) return reject("mesh has no vertices");
  const PositionDeformData position_data(depsgraph, object);
  const Span<float3> eval = position_data.eval;
  if (eval.size() != mesh.verts_num) return reject("evaluated vertex count differs from base mesh");
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const MeshAttributeData attributes(mesh);
  const double h = 1.0 / 60.0;

  struct Influence {
    int vertex;
    float factor;
    float3 original_position;
  };
  struct PreparedNode {
    int node_id;
    Vector<float3> translation;
  };
  std::vector<PreparedNode> writes;
  try {
    std::shared_ptr<sculpt_physics::StrokeSolverSession> session =
        std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(
            cache.physical_solver_session);
    if (!session || session->purpose != sculpt_physics::StrokeSolverPurpose::Grab ||
        session->vertex_count != std::size_t(mesh.verts_num))
    {
      sculpt_physics::BackendKind requested = sculpt_physics::BackendKind::Auto;
      if (const char *backend_name = std::getenv("BLENDER_SCULPT_XPBD_BACKEND")) {
        requested = sculpt_physics::backend_kind_from_name(backend_name);
      }
      session = sculpt_physics::create_stroke_solver_session(
          sculpt_physics::StrokeSolverPurpose::Grab, requested, std::size_t(mesh.verts_num));
      sculpt_physics::BrushSolverBackend &world = *session->solver;
      std::fprintf(stderr,
                   "[V12 XPBD Grab] backend selected: %s\n",
                   session->selected_backend == sculpt_physics::BackendKind::WarpCuda ?
                       "Warp CUDA" :
                       "CPU reference");
      if (requested == sculpt_physics::BackendKind::WarpCuda &&
          session->selected_backend != sculpt_physics::BackendKind::WarpCuda)
      {
        std::fprintf(stderr,
                     "[V12 XPBD Grab] Warp CUDA unavailable; using CPU reference backend\n");
      }
      for (const int i : eval.index_range()) {
        const float3 p = eval[i];
        const bool hidden = !attributes.hide_vert.is_empty() && attributes.hide_vert[i];
        const float mask = attributes.mask.is_empty() ? 0.0f : attributes.mask[i];
        const double inv_mass = hidden || mask >= 0.999f ? 0.0 :
                                std::max(1.0e-4, 1.0 - double(mask));
        world.add_particle({double(p.x), double(p.y), double(p.z)}, inv_mass);
      }
      float3 bounds_min = eval[0];
      float3 bounds_max = eval[0];
      for (const float3 &p : eval) {
        bounds_min = math::min(bounds_min, p);
        bounds_max = math::max(bounds_max, p);
      }
      const float3 extent = bounds_max - bounds_min;
      const double object_diagonal = std::max(
          1.0e-12,
          std::sqrt(double(extent.x) * extent.x + double(extent.y) * extent.y +
                    double(extent.z) * extent.z));
      double material_compliance = 1.0e-3;
      if (const char *value = std::getenv("BLENDER_SCULPT_XPBD_MATERIAL_COMPLIANCE")) {
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end != value && std::isfinite(parsed) && parsed >= 0.0) {
          material_compliance = parsed;
        }
      }
      /* Exact zero makes every irregular triangle edge a rigid constraint.
       * With an interactive iteration budget this is ill-conditioned and
       * appears as concentric stair-stepping. Preserve a very stiff result
       * while retaining enough compliance for stable convergence. */
      material_compliance = std::max(material_compliance, 1.0e-5);
      for (const int2 edge : mesh.edges()) {
        // Degenerate source edges do not define a valid material constraint.
        try {
          const float3 edge_delta = eval[edge[1]] - eval[edge[0]];
          const double edge_length = std::sqrt(double(edge_delta.x) * edge_delta.x +
                                               double(edge_delta.y) * edge_delta.y +
                                               double(edge_delta.z) * edge_delta.z);
          const double normalized_compliance =
              material_compliance * edge_length / object_diagonal;
          world.add_bond(std::size_t(edge[0]), std::size_t(edge[1]), normalized_compliance);
        }
        catch (const std::invalid_argument &) {
          continue;
        }
      }
      double damping = 60.0;
      if (const char *value = std::getenv("BLENDER_SCULPT_XPBD_DAMPING")) {
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end != value && std::isfinite(parsed) && parsed >= 0.0) {
          damping = parsed;
        }
      }
      world.set_velocity_damping(damping);
      world.begin_stroke();
      cache.physical_solver_session = session;
    }
    sculpt_physics::BrushSolverBackend &world = *session->solver;
    std::vector<Influence> selection;
    Vector<float> factors;
    Vector<float> distances;
    node_mask.foreach_index([&](const int ni) {
      const bke::pbvh::MeshNode &node = nodes[ni];
      const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);
      const Span<int> verts = node.verts();
      calc_factors_common_from_orig_data_mesh(depsgraph,
                                              brush,
                                              object,
                                              attributes,
                                              orig_data.positions,
                                              orig_data.normals,
                                              node,
                                              factors,
                                              distances);
      if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
        calc_silhouette_factors(cache, grab_delta, orig_data.normals, factors);
      }
      for (const int j : verts.index_range()) {
        if (factors[j] > 0.0f && std::isfinite(factors[j])) {
          selection.push_back(
              {verts[j], std::min(factors[j], 1.0f), orig_data.positions[j]});
        }
      }
    });
    std::sort(selection.begin(), selection.end(),
              [](const Influence &a, const Influence &b) { return a.vertex < b.vertex; });

    /* The gathered PBVH nodes are already covered by Sculpt Undo. Use all of
     * their vertices as a material halo while keeping direct Grab targets
     * limited to vertices with a positive brush factor. */
    std::vector<std::size_t> active_region;
    node_mask.foreach_index([&](const int ni) {
      for (const int vertex : nodes[ni].verts()) {
        active_region.push_back(std::size_t(vertex));
      }
    });
    std::sort(active_region.begin(), active_region.end());
    active_region.erase(std::unique(active_region.begin(), active_region.end()),
                        active_region.end());
    if (!world.set_active_region(active_region)) {
      throw std::runtime_error("XPBD rejected the Undo-covered material halo");
    }

    /* Physical Assist has two jobs: it controls the final material correction
     * and relaxes the Grab goal itself. A zero-compliance goal pins every
     * selected vertex to native Grab, leaving distance constraints, damping
     * and iterations no observable freedom even at 100% assist. */
    float physical_assist = 0.15f;
    if (const char *value = std::getenv("BLENDER_SCULPT_XPBD_GRAB_ASSIST")) {
      char *end = nullptr;
      const float parsed = std::strtof(value, &end);
      if (end != value && std::isfinite(parsed)) {
        physical_assist = std::clamp(parsed, 0.0f, 1.0f);
      }
    }
    /* Keep the goal only slightly compliant. Raw mesh-edge XPBD is
     * anisotropic on irregular triangulations; a large value exposes that
     * discretization as visible corrugation. */
    const double target_compliance = 1.0e-6 * double(physical_assist);

    std::vector<sculpt_physics::World::BrushTargetInput> targets;
    for (std::size_t k = 0; k < selection.size();) {
      const int vi = selection[k].vertex;
      float strength = selection[k].factor;
      float3 source = selection[k].original_position;
      while (++k < selection.size() && selection[k].vertex == vi) {
        if (selection[k].factor > strength) {
          strength = selection[k].factor;
          source = selection[k].original_position;
        }
      }
      /* Blender's Grab delta is cumulative from the beginning of the stroke.
       * Anchor it to the stroke-original position; using an event-to-event
       * delta makes a long drag behave as a sequence of disconnected taps. */
      const float3 goal = source + grab_delta * strength;
      targets.push_back(
          {std::size_t(vi), {double(goal.x), double(goal.y), double(goal.z)}, target_compliance});
    }
    if (targets.empty()) {
      return true;
    }
    world.set_targets(targets);
    // Only brush targets (hop=0) are dynamic. Crossing bonds react as
    // numerical pins; no exterior mesh vertex can change without an Undo node.
    world.step_local(h, 3, 10, 0);

    /* The V12 local adapter cannot publish motion outside Blender's Undo node
     * mask. A pure bond solve therefore behaves like rubber pinned at the
     * brush boundary. Keep a controlled physical correction while preserving
     * the native Grab target as the dominant result. */
    // Complete staging and float-range validation before any editor writeback.
    // Use the *same* PBVH node mask already handled by Blender Sculpt Undo.
    node_mask.foreach_index([&](const int ni) {
      PreparedNode prepared;
      prepared.node_id = ni;
      const Span<int> verts = nodes[ni].verts();
      prepared.translation.resize(verts.size());
      for (const int j : verts.index_range()) {
        sculpt_physics::Vec3 desired = world.particles()[verts[j]].position;
        const auto target = std::lower_bound(
            targets.begin(),
            targets.end(),
            std::size_t(verts[j]),
            [](const sculpt_physics::World::BrushTargetInput &candidate, const std::size_t vertex) {
              return candidate.particle_index < vertex;
            });
        if (target != targets.end() && target->particle_index == std::size_t(verts[j])) {
          sculpt_physics::Vec3 physical_correction = desired - target->goal;
          const double correction_length = std::sqrt(
              physical_correction.x * physical_correction.x +
              physical_correction.y * physical_correction.y +
              physical_correction.z * physical_correction.z);
          const double safe_radius = std::max(
              1.0e-12, double(math::length(grab_delta)) * 0.05 * double(physical_assist));
          if (correction_length > safe_radius) {
            physical_correction = physical_correction * (safe_radius / correction_length);
          }
          desired = target->goal + physical_correction * double(physical_assist);
        }
        const float3 old = eval[verts[j]];
        const float3 change{float(desired.x-double(old.x)),
                            float(desired.y-double(old.y)),
                            float(desired.z-double(old.z))};
        if (!std::isfinite(change.x) || !std::isfinite(change.y) || !std::isfinite(change.z)) {
          throw std::runtime_error("V12 experimental Grab displacement out of float range");
        }
        prepared.translation[j] = change;
      }
      clip_and_lock_translations(sd, ss, eval, verts, prepared.translation);
      writes.push_back(std::move(prepared));
    });
  }
  catch (const std::exception &ex) {
    // No mesh was changed: hand this dab back to the regular Grab brush,
    // and disable the experimental preview for the rest of this stroke.
    std::fprintf(stderr, "[V12 XPBD Grab] preview disabled: %s\n", ex.what());
    cache.physical_solver_session.reset();
    cache.xpbd_v12_failed = true;
    return false;
  }

  for (PreparedNode &prepared : writes) {
    bke::pbvh::MeshNode &node = nodes[prepared.node_id];
    position_data.deform(prepared.translation, node.verts());
    bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  if (!cache.xpbd_v12_active_logged) {
    std::fprintf(stderr, "[V12 XPBD Grab] experimental preview ACTIVE on ordinary mesh\n");
    cache.xpbd_v12_active_logged = true;
  }
  return true;
}

#ifdef WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14
/* V14 is a SEPARATE opt-in research path, deliberately independent from the
 * legacy V12/V13 World preview. Unlike V12, it exercises IPhysicsExecutor
 * through a strictly local, pinned one-ring island. It only runs on an
 * ordinary PBVH Mesh and relies on the existing native Grab Undo node mask.
 * This is not the final persistent CUDA or Multires integration. */
struct V14GrabState {
  sculpt_physics::World rest_world;
  std::unique_ptr<sculpt_physics::GpuTopologyBondIndex> topology;
  int verts_num = 0;
  int edges_num = 0;
};

static bool try_xpbd_executor_grab_preview(const Depsgraph &depsgraph,
                                           const Sculpt &sd,
                                           Object &object,
                                           const IndexMask &node_mask,
                                           const Brush &brush,
                                           const float3 &grab_delta,
                                           bke::pbvh::Tree &pbvh)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_XPBD_GRAB_EXECUTOR_V14");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') return false;
  if (pbvh.type() != bke::pbvh::Type::Mesh) return false;
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  const ePaintSymmetryFlags nontrivial_symmetry = ePaintSymmetryFlags(
      PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z |
      PAINT_TILE_X | PAINT_TILE_Y | PAINT_TILE_Z);
  // Never switch from incremental V14 goals to stock Grab's cumulative
  // displacement in the MIDDLE of one stroke: that could jump the mesh.
  // After a V14 failure, freeze this stroke until the user releases Grab.
  if (cache.xpbd_v14_failed) return cache.xpbd_v14_active_logged;
  if (ss.deform_modifiers_active || ss.shapekey_active ||
      bke::multires_layers::base_modifier(object) != nullptr ||
      (sd.paint.symmetry_flags & nontrivial_symmetry) != PAINT_SYMM_NONE ||
      cache.tile_pass != 0 || cache.radial_symmetry_pass != 0 ||
      cache.mirror_symmetry_pass != 0) {
    if (cache.xpbd_v14_active_logged) {
      cache.xpbd_v14_failed = true;
      return true;
    }
    return false;
  }

  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  if (mesh.verts_num <= 0) return false;
  const PositionDeformData position_data(depsgraph, object);
  const Span<float3> eval = position_data.eval;
  if (eval.size() != mesh.verts_num) return false;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const MeshAttributeData attributes(mesh);
  const float3 delta = grab_delta - cache.xpbd_v14_previous_delta;
  // No movement: keep the stroke state but do not build a rest proxy,
  // solve spring relaxation or clip/write any native geometry.
  if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) {
    return true;
  }
  struct PreparedNode {
    int node_id;
    Vector<float3> translation;
  };
  std::vector<PreparedNode> node_writes;
  sculpt_physics::PreparedNativeGrabDab prepared;
  std::shared_ptr<V14GrabState> state;

  // Staging must COMPLETE before any Blender PBVH geometry is modified.
  try {
    state = std::static_pointer_cast<V14GrabState>(cache.xpbd_v14_state);
    if (state && (state->verts_num != mesh.verts_num ||
                  state->edges_num != mesh.edges_num)) {
      throw std::runtime_error("V14 native Grab topology changed during a stroke");
    }
    if (!state) {
      state = std::make_shared<V14GrabState>();
      state->verts_num = mesh.verts_num;
      state->edges_num = mesh.edges_num;
      for (const int i : eval.index_range()) {
        const float3 p = eval[i];
        const bool hidden = !attributes.hide_vert.is_empty() && attributes.hide_vert[i];
        const float mask = attributes.mask.is_empty() ? 0.0f : attributes.mask[i];
        // Final native Grab factors already include partial mask influence.
        // Match V15: only fully masked/hidden vertices are fixed, rather than
        // attenuating the same partial mask again through inverse mass.
        const double inv_mass = hidden || mask >= 0.999f ? 0.0 : 1.0;
        state->rest_world.add_particle(
            {double(p.x), double(p.y), double(p.z)}, inv_mass);
      }
      for (const int2 edge : mesh.edges()) {
        try {
          state->rest_world.add_bond(
              std::size_t(edge[0]), std::size_t(edge[1]), 1.0e-5);
        }
        catch (const std::invalid_argument &) {
          // Degenerate source edge must not introduce an invalid constraint.
        }
      }
      state->topology = std::make_unique<sculpt_physics::GpuTopologyBondIndex>(
          state->rest_world, 1);
    }

    std::vector<sculpt_physics::NativeGrabSample> samples;
    Vector<float> factors;
    Vector<float> distances;
    node_mask.foreach_index([&](const int ni) {
      const bke::pbvh::MeshNode &node = nodes[ni];
      const OrigPositionData orig_data = orig_position_data_get_mesh(object, node);
      const Span<int> verts = node.verts();
      calc_factors_common_from_orig_data_mesh(depsgraph,
                                              brush,
                                              object,
                                              attributes,
                                              orig_data.positions,
                                              orig_data.normals,
                                              node,
                                              factors,
                                              distances);
      if (brush.flag2 & BRUSH_GRAB_SILHOUETTE) {
        calc_silhouette_factors(cache, grab_delta, orig_data.normals, factors);
      }
      for (const int j : verts.index_range()) {
        if (factors[j] > 0.0f && std::isfinite(factors[j])) {
          samples.push_back(
              {std::size_t(verts[j]), double(std::min(factors[j], 1.0f))});
        }
      }
    });
    std::sort(samples.begin(), samples.end(),
              [](const auto &a, const auto &b) {
                return a.global_index < b.global_index;
              });
    // PBVH nodes may carry repeated vertex IDs. The existing V12 behavior
    // keeps the largest final native factor, without multiplying it twice.
    std::vector<sculpt_physics::NativeGrabSample> unique;
    unique.reserve(samples.size());
    for (const auto &sample : samples) {
      if (!unique.empty() && unique.back().global_index == sample.global_index) {
        unique.back().factor = std::max(unique.back().factor, sample.factor);
      }
      else {
        unique.push_back(sample);
      }
    }
    sculpt_physics::NativeGrabDab dab;
    dab.delta = {double(delta.x), double(delta.y), double(delta.z)};
    dab.compliance = 1.0e-6;
    // Opt-in Grab stretch resistance. Smaller compliance scales are
    // stiffer; keep stroke-start rest lengths, unlike additive brushes.
    // Read once, so a running stroke cannot change its material.
    static const double bond_scale=[]() {
      const char *raw=std::getenv("BLENDER_SCULPT_XPBD_V14_BOND_SCALE");
      if (!raw) return 1.0;
      char *end=nullptr;
      const double value=std::strtod(raw,&end);
      if (end==raw || *end!='\0' || !std::isfinite(value) ||
          value<0.0 || value>1000.0) return 1.0;
      return value;
    }();
    dab.material.bond_compliance_scale=bond_scale;
    dab.dt = 1.0 / 60.0;
    dab.substeps = 2;
    dab.iterations = 5;
    dab.topology_generation = 1;

    prepared = sculpt_physics::prepare_cpu_native_grab_dab(
        state->rest_world,
        *state->topology,
        unique,
        [&](std::size_t vi) -> sculpt_physics::Vec3 {
          if (vi >= std::size_t(eval.size())) {
            throw std::out_of_range("V14 current PBVH vertex ID out of range");
          }
          const float3 p = eval[int(vi)];
          return {double(p.x), double(p.y), double(p.z)};
        },
        dab);

    if (prepared.writes().empty()) {
      // An empty or completely pinned dab must not clip zero translations,
      // recalculate every node bound, or dirty PBVH draw caches.
      prepared.finish_editor_success();
      cache.xpbd_v14_state = state;
      cache.xpbd_v14_previous_delta = grab_delta;
      return true;
    }

    std::unordered_map<std::size_t, float3> staged_translations;
    staged_translations.reserve(prepared.writes().size());
    for (const auto &write : prepared.writes()) {
      const float3 old = eval[int(write.global_index)];
      const float3 translation{
          float(write.position.x - double(old.x)),
          float(write.position.y - double(old.y)),
          float(write.position.z - double(old.z))};
      if (!std::isfinite(translation.x) || !std::isfinite(translation.y) ||
          !std::isfinite(translation.z)) {
        throw std::runtime_error("V14 native Grab result outside float position range");
      }
      if (!staged_translations.emplace(write.global_index, translation).second) {
        throw std::runtime_error("V14 duplicate writeback vertex");
      }
    }

    std::unordered_set<std::size_t> seen;
    seen.reserve(staged_translations.size());
    node_mask.foreach_index([&](const int ni) {
      PreparedNode node_write;
      node_write.node_id = ni;
      const Span<int> verts = nodes[ni].verts();
      node_write.translation.resize(verts.size());
      for (const int j : verts.index_range()) {
        const std::size_t vi=std::size_t(verts[j]);
        const auto it=staged_translations.find(vi);
        // If PBVH nodes share a boundary vertex, only the FIRST covered
        // node is allowed to deform that vertex. Subsequent nodes receive
        // zero displacement; this avoids applying a sparse edit twice.
        const bool first_owner=(it != staged_translations.end() &&
                                seen.insert(vi).second);
        node_write.translation[j]=first_owner ? it->second : float3(0.0f);
      }
      const Vector<float3> unclipped=node_write.translation;
      clip_and_lock_translations(sd, ss, eval, verts, node_write.translation);
      // A constrained/locked coordinate would invalidate the isolated
      // solver's acknowledged positions. Fail BEFORE editor modification and
      // return this dab to the original Blender Grab brush instead.
      for (const int j : verts.index_range()) {
        const float3 expected = unclipped[j];
        const float3 actual = node_write.translation[j];
        const float eps = 1.0e-6f * (1.0f + math::length(expected));
        if (std::abs(actual.x - expected.x) > eps ||
            std::abs(actual.y - expected.y) > eps ||
            std::abs(actual.z - expected.z) > eps) {
          throw std::runtime_error("V14 locked/mirror-clipped Grab: using stock brush");
        }
      }
      node_writes.push_back(std::move(node_write));
    });
    if (seen.size() != staged_translations.size()) {
      throw std::logic_error("V14 XPBD output is not fully covered by native Undo PBVH nodes");
    }
  }
  catch (const std::exception &ex) {
    // No Blender mesh write has occurred here; keep standard Grab intact.
    std::fprintf(stderr, "[V14 XPBD Grab] disabled: %s\n", ex.what());
    prepared.cancel();
    cache.xpbd_v14_state.reset();
    cache.xpbd_v14_failed = true;
    // Stock Grab uses a different cumulative displacement convention.
    // If V14 already wrote a previous dab, freeze until stroke end instead
    // of mixing two solvers with incompatible mid-stroke coordinates.
    return cache.xpbd_v14_active_logged;
  }

  // Native Blender Sculpt owns Undo and the actual PBVH writeback. These
  // operations currently have no full multi-node rollback hook. V14 remains
  // an explicitly enabled research preview, not production-ready physics.
  for (PreparedNode &node_write : node_writes) {
    bke::pbvh::MeshNode &node = nodes[node_write.node_id];
    position_data.deform(node_write.translation, node.verts());
    bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  prepared.finish_editor_success();
  cache.xpbd_v14_state = state;
  cache.xpbd_v14_previous_delta = grab_delta;
  if (!cache.xpbd_v14_active_logged) {
    std::fprintf(stderr, "[V14 XPBD Grab] native IPhysicsExecutor preview ACTIVE\n");
    cache.xpbd_v14_active_logged = true;
  }
  return true;
}
#endif
#endif

}  // namespace grab_cc

void do_grab_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &object,
                   const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  const float3 grab_delta = grab_delta_get(brush, *ss.cache) * ss.cache->bstrength;

#if defined(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL) && !defined(BSE_VOLUMETRIC_ONLY)
#  ifdef WITH_SCULPT_XPBD_GRAB_EXECUTOR_V14
  const char *v14 = std::getenv("BLENDER_SCULPT_XPBD_GRAB_EXECUTOR_V14");
  if (v14 && v14[0] == '1' && v14[1] == '\0') {
    if (try_xpbd_executor_grab_preview(
            depsgraph, sd, object, node_mask, brush, grab_delta, pbvh)) {
      return;
    }
  }
  else
#  endif
  {
    if (try_xpbd_grab_preview(depsgraph, sd, object, node_mask, brush, grab_delta, pbvh)) {
      return;
    }
  }
#endif

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_faces(depsgraph,
                       sd,
                       brush,
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
            calc_grids(depsgraph, sd, object, brush, grab_delta, nodes[i], tls);
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
            calc_bmesh(depsgraph, sd, object, brush, grab_delta, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}
}  // namespace ed::sculpt_paint::brushes

namespace ed::sculpt_paint {

void geometry_preview_lines_update(Depsgraph &depsgraph,
                                   Object &object,
                                   SculptSession &ss,
                                   float radius)
{
  ss.preview_verts = {};

  /* This function is called from the cursor drawing code, so the tree may not be built yet. */
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh) {
    return;
  }

  if (!ss.deform_modifiers_active) {
    return;
  }

  if (pbvh->type() != bke::pbvh::Type::Mesh) {
    return;
  }

  BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);

  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  /* Always grab active shape key if the sculpt happens on shapekey. */
  const Span<float3> positions = ss.shapekey_active ?
                                     bke::pbvh::vert_positions_eval(depsgraph, object) :
                                     mesh.vert_positions();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);

  const int active_vert = std::get<int>(ss.active_vert());
  const float3 brush_co = positions[active_vert];
  const float radius_sq = radius * radius;

  Vector<int> preview_verts;
  Vector<int> neighbors;
  BitVector<> visited_verts(positions.size());
  std::queue<int> queue;
  queue.push(active_vert);
  while (!queue.empty()) {
    const int from_vert = queue.front();
    queue.pop();

    neighbors.clear();
    for (const int neighbor : vert_neighbors_get_mesh(
             faces, corner_verts, vert_to_face_map, hide_poly, from_vert, neighbors))
    {
      preview_verts.append(from_vert);
      preview_verts.append(neighbor);
      if (visited_verts[neighbor]) {
        continue;
      }
      visited_verts[neighbor].set();
      if (math::distance_squared(brush_co, positions[neighbor]) < radius_sq) {
        queue.push(neighbor);
      }
    }
  }

  ss.preview_verts = preview_verts.as_span();
}

}  // namespace ed::sculpt_paint

}  // namespace blender
