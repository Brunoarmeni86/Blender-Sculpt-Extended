/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Clay Strips brush displaces vertices toward the brush plane.
 * The displacement occurs in the direction of the plane's normal.
 * Only vertices located below the plane (in brush-local space) are affected.
 *
 * The magnitude of the displacement is determined by the product of the following factors:
 * - A falloff factor based on the XY distance from the center of the plane
 * - A parabolic falloff factor based on the local Z distance from the plane
 * - Additional standard modifiers such as masking, brush strength, texture masking, etc.
 */

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>
#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_time.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "PRF_profile.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
#  include "physics_brush_v15.hh"
#endif
#include "editors/sculpt_paint/mesh/sculpt_layer_detail_v01.hh"

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
#  include "brush_solver_backend.hh"
#endif


namespace blender::ed::sculpt_paint::brushes {

inline namespace clay_strips_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float2> xy_positions;
  Vector<float> z_positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

/**
 * Transforms positions from object space positions to brush-local space. Splitting the XY and Z
 * components gives slightly better performance.
 */
static void calc_local_positions(const Span<float3> vert_positions,
                                 const Span<int> verts,
                                 const float4x4 &mat,
                                 const MutableSpan<float2> xy_positions,
                                 const MutableSpan<float> z_positions)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(xy_positions.size() == verts.size());
  BLI_assert(z_positions.size() == verts.size());

  for (const int i : verts.index_range()) {
    const float3 position = math::transform_point(mat, vert_positions[verts[i]]);

    xy_positions[i] = position.xy();
    z_positions[i] = position.z;
  }
}

static void calc_local_positions(const Span<float3> positions,
                                 const float4x4 &mat,
                                 const MutableSpan<float2> xy_positions,
                                 const MutableSpan<float> z_positions)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(xy_positions.size() == positions.size());
  BLI_assert(z_positions.size() == positions.size());

  for (const int i : positions.index_range()) {
    const float3 position = math::transform_point(mat, positions[i]);

    xy_positions[i] = position.xy();
    z_positions[i] = position.z;
  }
}

/**
 * Applies a parabolic factor of the form `z * (1 - z)` to each vertex.
 * Vertices outside of the interval (0, 1) are out of range and their factors are set to zero.
 * Note: The local coordinate system is constructed such that all relevant `z` values
 * are non-negative.
 */
static void apply_z_axis_factors(const Span<float> z_positions, const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(factors.size() == z_positions.size());

  for (const int i : factors.index_range()) {
    const float local_z = z_positions[i];

    /* Note: if `local_z > 1`, then `1 - local_z < 0` and the product is negative. */
    factors[i] *= math::max(0.0f, local_z * (1.0f - local_z));
  }
}

/**
 * If plane trim is enabled, vertices with `z` values greater than the
 * specified `plane_trim` threshold are ignored (i.e., their factors are set to zero).
 */
static void apply_plane_trim_factors(const Brush &brush,
                                     const Span<float> z_positions,
                                     const MutableSpan<float> factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(factors.size() == z_positions.size());

  const bool use_plane_trim = brush.flag & BRUSH_PLANE_TRIM;
  if (!use_plane_trim) {
    return;
  }

  for (const int i : factors.index_range()) {
    if (z_positions[i] > brush.plane_trim) {
      factors[i] = 0.0f;
    }
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float3 &offset,
                       const Span<float3> vert_normals,
                       const MeshAttributeData &attribute_data,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data,
                       const bool apply_deformation = true)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, position_data.eval, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.xy_positions.resize(verts.size());
  tls.z_positions.resize(verts.size());
  MutableSpan<float2> xy_positions = tls.xy_positions;
  MutableSpan<float> z_positions = tls.z_positions;

  calc_local_positions(position_data.eval, verts, mat, xy_positions, z_positions);
  apply_z_axis_factors(z_positions, factors);
  apply_plane_trim_factors(brush, z_positions, factors);

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_cube_distances<float2>(brush, xy_positions, distances);
  filter_distances_with_radius(1.0f, distances, factors);
  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               1.0f,
                               factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, position_data.eval, verts, factors);

  tls.translations.resize(verts.size());
  translations_from_offset_and_factors(offset, factors, tls.translations);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, tls.translations);
  if (apply_deformation) {
    position_data.deform(tls.translations, verts);
  }
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float3 &offset,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  tls.factors.resize(positions.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(subdiv_ccg, grids, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, subdiv_ccg, grids, factors);
  }

  tls.xy_positions.resize(positions.size());
  tls.z_positions.resize(positions.size());
  MutableSpan<float2> xy_positions = tls.xy_positions;
  MutableSpan<float> z_positions = tls.z_positions;

  calc_local_positions(positions, mat, xy_positions, z_positions);
  apply_z_axis_factors(z_positions, factors);
  apply_plane_trim_factors(brush, z_positions, factors);

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_cube_distances<float2>(brush, xy_positions, distances);
  filter_distances_with_radius(1.0f, distances, factors);
  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               1.0f,
                               factors);

  auto_mask::calc_grids_factors(depsgraph, object, cache.automasking.get(), node, grids, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  tls.translations.resize(positions.size());
  translations_from_offset_and_factors(offset, factors, tls.translations);

  clip_and_lock_translations(sd, ss, positions, tls.translations);
  apply_translations(tls.translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float4x4 &mat,
                       const float3 &offset,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(*ss.bm, verts, factors);
  filter_region_clip_factors(ss, positions, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, verts, factors);
  }

  tls.xy_positions.resize(positions.size());
  tls.z_positions.resize(positions.size());
  MutableSpan<float2> xy_positions = tls.xy_positions;
  MutableSpan<float> z_positions = tls.z_positions;

  calc_local_positions(positions, mat, xy_positions, z_positions);
  apply_z_axis_factors(z_positions, factors);
  apply_plane_trim_factors(brush, z_positions, factors);

  tls.distances.resize(positions.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_cube_distances<float2>(brush, xy_positions, distances);
  filter_distances_with_radius(1.0f, distances, factors);
  apply_hardness_to_distances(1.0f, cache.hardness, distances);
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               1.0f,
                               factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, positions, factors);

  tls.translations.resize(positions.size());
  translations_from_offset_and_factors(offset, factors, tls.translations);

  clip_and_lock_translations(sd, ss, positions, tls.translations);
  apply_translations(tls.translations, verts);
}

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
/* Physical Clay V01 is intentionally isolated and runtime opt-in. It uses the
 * standard Clay Strips displacement as an artistic collider/goal, then lets a
 * local elastoplastic surface proxy resist compression and retain material
 * continuity. Standard Clay remains byte-for-byte the default path. */
static bool try_physical_clay_v01(const Depsgraph &depsgraph,
                                  const Sculpt &sd,
                                  Object &object,
                                  const IndexMask &node_mask,
                                  const Brush &brush,
                                  const float4x4 &mat,
                                  const float3 &offset,
                                  const float3 &plane_normal,
                                  bke::pbvh::Tree &pbvh)
{
  const double physical_clay_start = BLI_time_now_seconds();
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  const char *enabled = std::getenv("BLENDER_SCULPT_PHYSICAL_CLAY_V01");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return false;
  }
  const auto reject = [&](const char *reason) {
    if (!cache.physical_clay_v01_rejection_logged) {
      std::fprintf(stderr, "[Physical Clay V01] standard fallback: %s\n", reason);
      cache.physical_clay_v01_rejection_logged = true;
    }
    return false;
  };
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return reject("initial prototype supports ordinary mesh PBVH only");
  }
  if (cache.physical_clay_v01_failed) {
    return reject("solver failed earlier in this stroke");
  }
  if (ss.deform_modifiers_active || ss.shapekey_active) {
    return reject("deform modifier or Shape Key is active");
  }
  if (cache.tile_pass != 0 || cache.radial_symmetry_pass != 0 ||
      int(cache.mirror_symmetry_pass) != 0)
  {
    return reject("V01 supports the primary symmetry pass only");
  }

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  if (mesh.verts_num == 0) {
    return reject("mesh has no vertices");
  }
  const PositionDeformData position_data(depsgraph, object, true);
  if (position_data.eval.size() != mesh.verts_num) {
    return reject("evaluated vertex count differs from base mesh");
  }
  const MeshAttributeData attributes(mesh);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const auto unit_setting = [](const char *name, const float fallback) {
    const char *value = std::getenv(name);
    if (!value) {
      return fallback;
    }
    char *end = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value && std::isfinite(parsed) ? std::clamp(parsed, 0.0f, 1.0f) : fallback;
  };
  float compression = unit_setting("BLENDER_SCULPT_PHYSICAL_CLAY_COMPRESSION", 0.65f);
  float plasticity = unit_setting("BLENDER_SCULPT_PHYSICAL_CLAY_PLASTICITY", 0.60f);
  float drag = unit_setting("BLENDER_SCULPT_PHYSICAL_CLAY_DRAG", 0.35f);
  float volume = unit_setting("BLENDER_SCULPT_PHYSICAL_CLAY_VOLUME", 0.50f);
  float recovery = unit_setting("BLENDER_SCULPT_PHYSICAL_CLAY_RECOVERY", 0.20f);
  enum class MaterialModel { Clay, Wax, Mud };
  MaterialModel material_model = MaterialModel::Clay;
  if (const char *material = std::getenv("BLENDER_SCULPT_PHYSICAL_CLAY_MATERIAL")) {
    if (STREQ(material, "wax")) {
      material_model = MaterialModel::Wax;
      compression = std::max(compression, 0.95f);
      plasticity = std::min(plasticity, 0.18f);
      drag = std::min(drag, 0.12f);
      volume = std::max(volume, 0.85f);
      recovery = std::max(recovery, 0.55f);
    }
    else if (STREQ(material, "mud")) {
      material_model = MaterialModel::Mud;
      compression = std::min(compression, 0.22f);
      plasticity = std::max(plasticity, 0.95f);
      drag = std::max(drag, 0.90f);
      volume = std::min(volume, 0.28f);
      recovery = std::min(recovery, 0.05f);
    }
  }

  sculpt_physics::BackendKind requested_backend = sculpt_physics::BackendKind::Auto;
  if (const char *backend_name = std::getenv("BLENDER_SCULPT_XPBD_BACKEND")) {
    requested_backend = sculpt_physics::backend_kind_from_name(backend_name);
  }
  /* Persistent sessions retain bonds, rest lengths and damping. Reusing one
   * after changing Clay/Wax/Mud (or a material slider) made all presets keep
   * the first selected material law. Hash the complete effective profile so
   * the session is rebuilt only when its physical configuration changes. */
  std::uint64_t material_configuration = 1469598103934665603ULL;
  const auto hash_setting = [&](const std::uint64_t value) {
    material_configuration ^= value;
    material_configuration *= 1099511628211ULL;
  };
  hash_setting(std::uint64_t(material_model));
  hash_setting(std::uint64_t(requested_backend));
  hash_setting(std::uint64_t(std::lround(compression * 100000.0f)));
  hash_setting(std::uint64_t(std::lround(plasticity * 100000.0f)));
  hash_setting(std::uint64_t(std::lround(drag * 100000.0f)));
  hash_setting(std::uint64_t(std::lround(volume * 100000.0f)));
  hash_setting(std::uint64_t(std::lround(recovery * 100000.0f)));

  struct Influence {
    int vertex;
    float factor;
    float3 translation;
  };
  struct PreparedNode {
    int node;
    Vector<float3> translations;
  };
  std::vector<PreparedNode> writes;
  std::size_t logged_targets = 0;
  std::size_t logged_region = 0;
  bool synchronize_compact_dab = false;
  try {
    std::shared_ptr<sculpt_physics::StrokeSolverSession> session =
        std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(
            cache.physical_solver_session);
    if (!session) {
      session = std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(
          ss.physical_solver_session_cache);
    }
    if (!session || session->purpose != sculpt_physics::StrokeSolverPurpose::PhysicalClay ||
        session->vertex_count != std::size_t(mesh.verts_num) ||
        session->configuration_key != material_configuration)
    {
      session = sculpt_physics::create_stroke_solver_session(
          sculpt_physics::StrokeSolverPurpose::PhysicalClay,
          requested_backend,
          std::size_t(mesh.verts_num));
      session->configuration_key = material_configuration;
      sculpt_physics::BrushSolverBackend &world = *session->solver;
      /* Structural edges plus at most one cross-face constraint per manifold
       * edge. Reserving the final topology size avoids a large allocation
       * cascade during the first physical dab on dense meshes. */
      world.reserve_capacity(std::size_t(mesh.verts_num),
                             std::size_t(mesh.edges_num) * 2);
      std::fprintf(stderr,
                   "[Physical Clay A3] backend selected: %s%s\n",
                   session->selected_backend == sculpt_physics::BackendKind::WarpCuda ?
                       "Warp CUDA" :
                       "CPU reference",
                   requested_backend == sculpt_physics::BackendKind::WarpCuda &&
                           session->selected_backend != sculpt_physics::BackendKind::WarpCuda ?
                       " (CUDA unavailable; fallback)" :
                       "");
      for (const int vertex : position_data.eval.index_range()) {
        const bool hidden = !attributes.hide_vert.is_empty() && attributes.hide_vert[vertex];
        const float mask = attributes.mask.is_empty() ? 0.0f : attributes.mask[vertex];
        const double inverse_mass = hidden || mask >= 0.999f ? 0.0 :
                                    std::max(1.0e-4, 1.0 - double(mask));
        const float3 p = position_data.eval[vertex];
        world.add_particle({double(p.x), double(p.y), double(p.z)}, inverse_mass);
      }
      float3 minimum = position_data.eval.first();
      float3 maximum = minimum;
      for (const float3 &p : position_data.eval) {
        minimum = math::min(minimum, p);
        maximum = math::max(maximum, p);
      }
      const double diagonal = std::max(1.0e-12, double(math::length(maximum - minimum)));
      double edge_length_sum = 0.0;
      std::size_t valid_edge_count = 0;
      for (const int2 edge : mesh.edges()) {
        const double length = double(math::distance(position_data.eval[edge[0]],
                                                     position_data.eval[edge[1]]));
        if (length > 1.0e-12 && std::isfinite(length)) {
          edge_length_sum += length;
          valid_edge_count++;
        }
      }
      const double mean_edge_length = valid_edge_count > 0 ?
                                          edge_length_sum / double(valid_edge_count) :
                                          diagonal;
      session->topology_mean_edge_length = mean_edge_length;
      const double material_compliance = 2.0e-3 * double(1.0f - compression) +
                                         2.5e-5 * double(compression);
      const double yield_strain = 0.18 * double(1.0f - plasticity) +
                                  0.025 * double(plasticity);
      const double plastic_creep = (0.5 + 8.0 * double(plasticity)) *
                                   (1.0 - 0.70 * double(recovery));
      for (const int2 edge : mesh.edges()) {
        const double length = double(math::distance(position_data.eval[edge[0]],
                                                     position_data.eval[edge[1]]));
        if (length <= 1.0e-12) {
          continue;
        }
        /* A finite yield and creep turn persistent compression into clay-like
         * rest-shape change while small strains remain elastic. */
        world.add_bond(std::size_t(edge[0]),
                       std::size_t(edge[1]),
                        material_compliance *
                            std::clamp(length / mean_edge_length, 0.25, 4.0),
                       yield_strain,
                       plastic_creep);
      }
      /* Add a topology-derived cross-face distance constraint. For two faces
       * sharing an edge this connects their opposite vertices, providing a
       * compact bend/shear approximation that is supported by both existing
       * CPU XPBD and Warp CUDA backends. Boundary and non-manifold edges are
       * left unchanged. This is deliberately less rigid than structural
       * edges; material profiles select different elastic/plastic response. */
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int> corner_verts = mesh.corner_verts();
      const Span<int> corner_edges = mesh.corner_edges();
      std::vector<int> first_opposite(std::size_t(mesh.edges_num), -1);
      std::vector<int> second_opposite(std::size_t(mesh.edges_num), -1);
      std::vector<unsigned char> edge_face_count(std::size_t(mesh.edges_num), 0);
      for (const int face_i : faces.index_range()) {
        const IndexRange face = faces[face_i];
        if (face.size() < 3) {
          continue;
        }
        for (const int local_corner : face.index_range()) {
          const int corner = face[local_corner];
          const int edge_i = corner_edges[corner];
          if (edge_i < 0 || edge_i >= mesh.edges_num) {
            continue;
          }
          const int opposite = corner_verts[face[(local_corner + 2) % face.size()]];
          if (edge_face_count[edge_i] == 0) {
            first_opposite[edge_i] = opposite;
          }
          else if (edge_face_count[edge_i] == 1) {
            second_opposite[edge_i] = opposite;
          }
          edge_face_count[edge_i] = std::min<unsigned char>(edge_face_count[edge_i] + 1, 3);
        }
      }
      for (const int edge_i : IndexRange(mesh.edges_num)) {
        if (edge_face_count[edge_i] != 2) {
          continue;
        }
        const int other = first_opposite[edge_i];
        const int opposite = second_opposite[edge_i];
        if (other < 0 || opposite < 0 || other == opposite) {
          continue;
        }
        const double diagonal_length = double(
            math::distance(position_data.eval[other], position_data.eval[opposite]));
          if (diagonal_length <= 1.0e-12 || !std::isfinite(diagonal_length)) {
            continue;
          }
          const double bend_scale = material_model == MaterialModel::Wax ? 6.0 :
                                    material_model == MaterialModel::Mud ? 30.0 :
                                                                          12.0;
          const double bend_yield = material_model == MaterialModel::Wax ?
                                          std::numeric_limits<double>::infinity() :
                                      material_model == MaterialModel::Mud ?
                                          yield_strain * 0.75 :
                                          yield_strain * 1.5;
          const double bend_creep = material_model == MaterialModel::Wax ? 0.0 :
                                       material_model == MaterialModel::Mud ?
                                           plastic_creep * 0.75 :
                                           plastic_creep * 0.35;
          world.add_bond(std::size_t(other),
                         std::size_t(opposite),
                         material_compliance * bend_scale *
                             std::clamp(diagonal_length / mean_edge_length, 0.5, 8.0),
                         bend_yield,
                         bend_creep);
      }
      /* Recovery controls shape memory; drag controls how freely material
       * follows the stroke. The CUDA backend now applies the same explicit
       * material intent rather than relying on one fixed global stiffness. */
      world.set_shape_stiffness_override(0.10 + 0.45 * double(recovery) +
                                         0.12 * double(compression));
      world.set_velocity_damping(22.0 + 58.0 * double(1.0f - drag));
      world.begin_stroke();
      cache.physical_solver_session = session;
      if (session->selected_backend == sculpt_physics::BackendKind::WarpCuda) {
        ss.physical_solver_session_cache = session;
      }
    }
    else {
      cache.physical_solver_session = session;
      /* Defer synchronization until the exact influenced set is known. A
       * full-mesh upload here dominated warm dabs on dense meshes and also
       * replaced the solver rest state outside the brush for no reason. */
      synchronize_compact_dab = session->selected_backend ==
                                sculpt_physics::BackendKind::WarpCuda;
    }
    sculpt_physics::BrushSolverBackend &world = *session->solver;

    std::vector<Influence> influences;
    LocalData local;
    float3 tangential = cache.grab_delta_symm -
                        plane_normal * math::dot(cache.grab_delta_symm, plane_normal);
    const float tangential_limit = cache.radius * 0.08f;
    const float tangential_length = math::length(tangential);
    if (tangential_length > tangential_limit && tangential_length > 0.0f) {
      tangential *= tangential_limit / tangential_length;
    }
    tangential *= 0.45f * drag;
    node_mask.foreach_index([&](const int node_i) {
      const bke::pbvh::MeshNode &node = nodes[node_i];
      calc_faces(depsgraph,
                 sd,
                 brush,
                 mat,
                 offset,
                 normals,
                 attributes,
                 node,
                 object,
                 local,
                 position_data,
                 false);
      const Span<int> verts = node.verts();
      for (const int i : verts.index_range()) {
        const float factor = local.factors[i];
        if (factor > 0.0f && std::isfinite(factor)) {
          const float3 current = position_data.eval[verts[i]];
          float3 radial = current - cache.location_symm;
          radial -= plane_normal * math::dot(radial, plane_normal);
          const float radial_length = math::length(radial);
          if (radial_length > 1.0e-12f) {
            radial /= radial_length;
          }
          else {
            radial = float3(0.0f);
          }
          const float compression_distance = math::abs(
              math::dot(local.translations[i], plane_normal));
          const float edge_band = std::clamp(4.0f * factor * (1.0f - factor), 0.0f, 1.0f);
          /* Cohesive rim accumulation is bounded by brush scale so dense
           * meshes do not receive a larger physical impulse merely because
           * more samples fall inside the dab. */
          const float accumulation_limit = cache.radius * 0.0125f;
          const float accumulation_amount = std::min(compression_distance * volume *
                                                         compression * 0.75f * edge_band,
                                                     accumulation_limit);
          const float3 accumulation = radial * accumulation_amount;
          influences.push_back(
              {verts[i], factor, local.translations[i] + tangential * factor + accumulation});
        }
      }
    });
    std::sort(influences.begin(), influences.end(), [](const Influence &a, const Influence &b) {
      return a.vertex < b.vertex;
    });

    std::vector<std::size_t> active_region;
    node_mask.foreach_index([&](const int node_i) {
      for (const int vertex : nodes[node_i].verts()) {
        active_region.push_back(std::size_t(vertex));
      }
    });
    std::sort(active_region.begin(), active_region.end());
    active_region.erase(std::unique(active_region.begin(), active_region.end()),
                        active_region.end());
    std::vector<sculpt_physics::World::BrushTargetInput> targets;
    std::vector<std::size_t> material_smooth_vertices;
    std::vector<float> material_smooth_weights;
    std::vector<sculpt_physics::Vec3> material_smooth_normals;
    for (std::size_t i = 0; i < influences.size();) {
      const int vertex = influences[i].vertex;
      Influence strongest = influences[i];
      while (++i < influences.size() && influences[i].vertex == vertex) {
        if (influences[i].factor > strongest.factor) {
          strongest = influences[i];
        }
      }
      const float3 goal = position_data.eval[vertex] + strongest.translation;
      targets.push_back({std::size_t(vertex),
                         {double(goal.x), double(goal.y), double(goal.z)},
                         2.0e-6 + 1.5e-4 * double(recovery)});
      material_smooth_vertices.push_back(std::size_t(vertex));
      const float smooth_amount = material_model == MaterialModel::Wax ? 0.38f :
                                  material_model == MaterialModel::Mud ? 0.55f :
                                                                       0.0f;
      material_smooth_weights.push_back(strongest.factor * smooth_amount);
      const float3 normal = normals[vertex];
      material_smooth_normals.push_back(
          {double(normal.x), double(normal.y), double(normal.z)});
    }
    if (targets.empty()) {
      return true;
    }
    logged_targets = targets.size();
    logged_region = active_region.size();
    bool compact_cuda_region = false;
    /* Large dense PBVH leaves can make the Undo-covered node halo many times
     * larger than the visible brush. On CUDA the full halo adds solver work
     * without improving a one-dab interactive result. Use the exact influenced
     * vertices as a bounded high-density proxy; CPU and smaller regions keep
     * the wider material halo. */
    if (world.kind() == sculpt_physics::BackendKind::WarpCuda) {
      compact_cuda_region = true;
      active_region.clear();
      active_region.reserve(targets.size());
      for (const sculpt_physics::World::BrushTargetInput &target : targets) {
        active_region.push_back(target.particle_index);
      }
      logged_region = active_region.size();
    }
    if (synchronize_compact_dab) {
      std::vector<sculpt_physics::Vec3> &current = session->compact_positions;
      current.clear();
      current.reserve(active_region.size());
      for (const std::size_t vertex : active_region) {
        const float3 p = position_data.eval[int(vertex)];
        current.push_back({double(p.x), double(p.y), double(p.z)});
      }
      if (!world.synchronize_positions_compact(active_region, current)) {
        throw std::runtime_error("could not synchronize compact Clay GPU positions");
      }
    }
    /* Apply the final region after the CUDA compacting decision. Previously
     * the wider PBVH node halo remained active even after active_region was
     * replaced by the exact influenced set. */
    if (!world.set_active_region(active_region)) {
      throw std::runtime_error("solver rejected the compact material region");
    }
    if (!world.set_targets(targets)) {
      throw std::runtime_error("solver rejected Clay targets");
    }
    /* A fixed Jacobi stiffness becomes visually weaker as the number of
     * samples across the same world-space brush radius grows. Normalize the
     * material term using the cached topology scale. This changes solver
     * strength rather than adding expensive full-region passes. Wax retains
     * the strongest elastic response, Mud the smallest shape-memory boost. */
    const double samples_across_radius = cache.radius /
        std::max(session->topology_mean_edge_length, 1.0e-12);
    const double density_factor = std::clamp(
        (std::log2(std::max(samples_across_radius, 1.0)) - 5.0) / 5.0, 0.0, 1.0);
    const double density_boost = material_model == MaterialModel::Wax ? 0.20 :
                                 material_model == MaterialModel::Mud ? 0.06 :
                                                                       0.12;
    world.set_shape_stiffness_override(
        std::clamp(0.10 + 0.45 * double(recovery) + 0.12 * double(compression) +
                       density_boost * density_factor,
                   0.05,
                   0.92));
    /* V02 interactive budget: the safety-clamped artistic goal remains the
     * dominant result, so a small quasistatic solve is sufficient. */
    /* Density-adaptive solve budget. PBVH node size, brush radius and mesh
     * density can make the same visual dab vary by orders of magnitude. Keep
     * high quality for small regions while bounding latency on million-vertex
     * meshes. The material parameters and safety clamps remain identical. */
    int substeps = 2;
    int iterations = 5;
    int bond_hops = 2;
    if (active_region.size() > 12000) {
      substeps = 1;
      iterations = 5;
      bond_hops = 1;
    }
    if (active_region.size() > 50000) {
      iterations = 4;
    }
    if (active_region.size() > 150000) {
      iterations = 3;
    }
    world.step_local(1.0 / 60.0, substeps, iterations, bond_hops);

    /* Clay intentionally keeps the precise artistic collider response. Wax
     * and Mud receive a real material regularization pass on the solved
     * surface: Wax preserves the normal profile, while Mud diffuses over
     * three compact passes to behave like a softer continuous mass. This is
     * topology-aware and density-normalized, unlike merely changing the same
     * five scalar parameters for every preset. */
    if (world.kind() == sculpt_physics::BackendKind::WarpCuda &&
        material_model != MaterialModel::Clay)
    {
      const sculpt_physics::SmoothSolverMode relaxation =
          material_model == MaterialModel::Wax ?
              sculpt_physics::SmoothSolverMode::VolumePreserve :
              sculpt_physics::SmoothSolverMode::DenseStrong;
      if (!world.smooth_compact(material_smooth_vertices,
                                material_smooth_weights,
                                material_smooth_normals,
                                relaxation,
                                double(cache.radius)))
      {
        throw std::runtime_error("Clay material relaxation failed");
      }
    }

    node_mask.foreach_index([&](const int node_i) {
      PreparedNode prepared;
      prepared.node = node_i;
      const Span<int> verts = nodes[node_i].verts();
      prepared.translations.resize(verts.size());
      for (const int i : verts.index_range()) {
        const auto target = std::lower_bound(
            targets.begin(),
            targets.end(),
            std::size_t(verts[i]),
            [](const sculpt_physics::World::BrushTargetInput &candidate,
               const std::size_t vertex) { return candidate.particle_index < vertex; });
        const sculpt_physics::Vec3 p = world.particles()[verts[i]].position;
        const float3 current = position_data.eval[verts[i]];
        float3 delta(float(p.x) - current.x, float(p.y) - current.y, float(p.z) - current.z);
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) {
          throw std::runtime_error("non-finite Physical Clay displacement");
        }
        /* Halo vertices are allowed to move only inside the Undo-covered PBVH
         * region, and receive a strict per-dab safety bound. */
        if (target == targets.end() || target->particle_index != std::size_t(verts[i])) {
          if (compact_cuda_region) {
            /* Vertices outside the compact solve were not updated this dab.
             * Publishing their stroke-start solver positions creates PBVH
             * rectangles and cracks on dense meshes. The one-ring topology
             * remains available to the solver as a read-only collar. */
            prepared.translations[i] = float3(0.0f);
            continue;
          }
          const float halo_limit = cache.radius * (0.0025f + 0.0175f * volume);
          const float halo_length = math::length(delta);
          if (halo_length > halo_limit && halo_length > 0.0f) {
            delta *= halo_limit / halo_length;
          }
          prepared.translations[i] = delta;
          continue;
        }
        const float3 artistic(float(target->goal.x) - current.x,
                              float(target->goal.y) - current.y,
                              float(target->goal.z) - current.z);
        const float safe_length = std::max(math::length(artistic) * 1.25f,
                                           cache.radius * 1.0e-4f);
        const float physical_length = math::length(delta);
        if (physical_length > safe_length && physical_length > 0.0f) {
          delta *= safe_length / physical_length;
        }
        prepared.translations[i] = delta;
      }
      clip_and_lock_translations(sd, ss, position_data.eval, verts, prepared.translations);
      writes.push_back(std::move(prepared));
    });
  }
  catch (const std::exception &error) {
    std::fprintf(stderr, "[Physical Clay V01] disabled: %s\n", error.what());
    cache.physical_solver_session.reset();
    ss.physical_solver_session_cache.reset();
    cache.physical_clay_v01_failed = true;
    return false;
  }

  for (PreparedNode &prepared : writes) {
    bke::pbvh::MeshNode &node = nodes[prepared.node];
    position_data.deform(prepared.translations, node.verts());
    bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  if (!cache.physical_clay_v01_active_logged) {
    std::fprintf(stderr, "[Physical Clay V01] CPU elastoplastic preview ACTIVE\n");
    cache.physical_clay_v01_active_logged = true;
  }
  const double physical_clay_ms = (BLI_time_now_seconds() - physical_clay_start) * 1000.0;
  if (physical_clay_ms >= 8.0) {
    std::fprintf(stderr,
                 "[Physical Clay A3] total=%.3f ms nodes=%lld targets=%zu region=%zu\n",
                 physical_clay_ms,
                 static_cast<long long>(node_mask.size()),
                 logged_targets,
                 logged_region);
  }
  return true;
}
#endif

}  // namespace clay_strips_cc

/* Sculpt Mesh V3 Deep Probe: diagnostics only, enabled unless explicitly set to 0. */
static bool sculpt_mesh_v3deep_enabled()
{
  static const bool enabled = []() {
    const char *value = std::getenv("BLENDER_SCULPT_V3_DEEP_PROBE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

void do_clay_strips_brush(const Depsgraph &depsgraph,
                          const Sculpt &sd,
                          Object &object,
                          const IndexMask &node_mask,
                          const float3 &plane_normal,
                          const float3 &plane_center)
{
  /* Sculpt Mesh V3 Clay memory probe.
   * Diagnostic only; split the regular Mesh path without changing brush math. */
  const double sculpt_mesh_v3_clay_total_start = BLI_time_now_seconds();
  double sculpt_mesh_v3_clay_setup_ms = 0.0;
  double sculpt_mesh_v3_clay_loop_ms = 0.0;
  bool sculpt_mesh_v3_clay_mesh_path = false;
  const bool v3deep = sculpt_mesh_v3deep_enabled();
  double v3deep_deform_ms = 0.0, v3deep_attributes_ms = 0.0, v3deep_normals_ms = 0.0;
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  if (physics_v15::freeze_if_mode_changed(object,pbvh,brush)) return;
#endif
  const bool flip = (ss.cache->bstrength < 0.0f);

  /* Note: This return has to happen *after* the call to calc_brush_plane for now, as
   * the method is not idempotent and sets variables inside the stroke cache. */
  const bool stationary=math::is_zero(ss.cache->grab_delta_symm);
  bool allow_stationary=false;
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  allow_stationary=physics_v15::requested(object,brush) &&
      physics_v15::autonomous_requested(*ss.cache) &&
      (physics_v15::fast_v26b_requested() || ss.cache->xpbd_v16_clay_behavior>=7 ||
       (physics_v15::drag_contact_requested() &&
        (ss.cache->xpbd_v16_clay_behavior==2 || ss.cache->xpbd_v16_clay_behavior==5))) &&
      pbvh.type()==bke::pbvh::Type::Mesh && !layer_detail_v01::state().active;
#endif
  if (stationary && !allow_stationary) return;

  const float4x4 mat = clay_strips::calc_local_matrix(
      brush, *ss.cache, plane_normal, plane_center, flip);
  const float3 offset = plane_normal * ss.cache->bstrength * ss.cache->radius;

#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  // The existing Clay Strips layer-detail projection must not be bypassed
  // by a base-mesh XPBD writer which has no layer-delta implementation.
  const bool v15_layer_detail=layer_detail_v01::state().active;
  if (physics_v15::requested(object,brush) && v15_layer_detail &&
      ss.cache->xpbd_v15_active_logged) {
    ss.cache->xpbd_v15_failed=true;
    return; // freeze an already physical stroke; do not mix editors
  }
  if (physics_v15::requested(object,brush) && !v15_layer_detail &&
      pbvh.type()==bke::pbvh::Type::Mesh) {
    Mesh &mesh=*id_cast<Mesh *>(object.data);
    const PositionDeformData pos(depsgraph,object,true);
    const MeshAttributeData attributes(mesh);
    const Span<float3> normals=bke::pbvh::vert_normals_eval(depsgraph,object);
    MutableSpan<bke::pbvh::MeshNode> nodes=pbvh.nodes<bke::pbvh::MeshNode>();
    std::vector<sculpt_physics::NativeBrushTranslation> native;
    std::vector<sculpt_physics::PhysicalBrushIntentSample> intent_samples;
    const bool autonomous=physics_v15::autonomous_requested(*ss.cache);
    const bool round_contact=autonomous && (physics_v15::shape_v24_requested() || ss.cache->xpbd_v16_clay_behavior==14) &&
        ss.cache->xpbd_v16_clay_behavior!=1 && ss.cache->xpbd_v16_clay_behavior!=4;
    const bool footprint=autonomous && physics_v15::footprint_v30e_brush(object,brush);
    const bool audit=footprint && physics_v15::profile_v26b_requested();
    physics_v15::FootprintAuditV30E coverage;
    LocalData tls;
    node_mask.foreach_index([&](const int ni) {
      const bke::pbvh::MeshNode &node=nodes[ni];
      const Span<int> verts=node.verts();
      tls.factors.resize(verts.size());
      MutableSpan<float> factors=tls.factors;
      tls.xy_positions.resize(verts.size());
      tls.z_positions.resize(verts.size());
      calc_local_positions(pos.eval,verts,mat,tls.xy_positions,tls.z_positions);
      tls.distances.resize(verts.size());
      MutableSpan<float> distances=tls.distances;
      if(footprint){
        for(const int j:verts.index_range())distances[j]=physics_v15::footprint_distance_v30e(*ss.cache,brush,pos.eval[verts[j]],plane_normal)/std::max(ss.cache->radius,1e-12f);
      }else if (round_contact) {
        const float3 axis=math::normalize(plane_normal);
        for (const int j:verts.index_range()) {
          const float3 d=pos.eval[verts[j]]-ss.cache->location_symm;
          const float3 tangent=d-axis*math::dot(d,axis);
          const float dist=physics_v15::swept_v29_active(*ss.cache,brush)?
              physics_v15::swept_distance_v29(*ss.cache,pos.eval[verts[j]],&axis):math::length(tangent);
          distances[j]=dist/std::max(ss.cache->radius,1e-12f);
        }
      }
      else calc_brush_cube_distances<float2>(brush,tls.xy_positions,distances);
      if(audit)for(const float dist:distances)if(dist<1.0f)++coverage.geometric;
      fill_factor_from_hide_and_mask(attributes.hide_vert,attributes.mask,verts,factors);
      filter_region_clip_factors(ss,pos.eval,verts,factors);
      if(audit)coverage.base+=coverage.count(factors,distances,1.0f);
      if(brush.flag & BRUSH_FRONTFACE)calc_front_face(ss.cache->view_normal_symm,normals,verts,factors);
      if(audit)coverage.front+=coverage.count(factors,distances,1.0f);
      if(!autonomous)apply_z_axis_factors(tls.z_positions,factors);
      apply_plane_trim_factors(brush,tls.z_positions,factors);
      if(audit)coverage.trim+=coverage.count(factors,distances,1.0f);
      filter_distances_with_radius(1.0f,distances,factors);
      apply_hardness_to_distances(1.0f,ss.cache->hardness,distances);
      BKE_brush_calc_curve_factors(
          eBrushCurvePreset(brush.curve_distance_falloff_preset),
          brush.curve_distance_falloff,distances,1.0f,factors);
      if(audit)coverage.curve+=coverage.count(factors,distances,1.0f);
      auto_mask::calc_vert_factors(
          depsgraph,object,ss.cache->automasking.get(),node,verts,factors);
      if(audit)coverage.automask+=coverage.count(factors,distances,1.0f);
      calc_brush_texture_factors(ss,brush,pos.eval,verts,factors);
      if(audit)coverage.texture+=coverage.count(factors,distances,1.0f);
      if (autonomous) {
        physics_v15::append_intent_samples(intent_samples,verts,pos.eval,factors,normals);
        return;
      }
      tls.translations.resize(verts.size());
      translations_from_offset_and_factors(offset,factors,tls.translations);
      for (const int j : verts.index_range()) {
        const float3 value=tls.translations[j];
        native.push_back({std::size_t(verts[j]),
                          {double(value.x),double(value.y),double(value.z)}});
      }
    });
    if(audit)coverage.report(ss.cache->xpbd_v16_clay_behavior);
    if (autonomous ?
        physics_v15::try_apply_intent(sd,object,node_mask,pos,
                                    sculpt_physics::NativeSculptBrushKind::ClayStrips,
                                    intent_samples,plane_normal) :
        physics_v15::try_apply(sd,object,node_mask,pos,
                              sculpt_physics::NativeSculptBrushKind::ClayStrips,
                              native)) {
      return;
    }
  }
#endif

  if (stationary) return; // no degenerate native fallback after stationary trial
#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
  if (try_physical_clay_v01(
          depsgraph, sd, object, node_mask, brush, mat, offset, plane_normal, pbvh))
  {
    return;
  }
#endif

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      sculpt_mesh_v3_clay_mesh_path = true;
      const double sculpt_mesh_v3_clay_setup_start = BLI_time_now_seconds();      const double v3deep_deform_start = v3deep ? BLI_time_now_seconds() : 0.0;
      const PositionDeformData position_data(depsgraph, object, true);
      const double v3deep_attributes_start = v3deep ? BLI_time_now_seconds() : 0.0;
      const MeshAttributeData attribute_data(mesh);
      const double v3deep_normals_start = v3deep ? BLI_time_now_seconds() : 0.0;
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      if (v3deep) {
        const double end = BLI_time_now_seconds();
        v3deep_deform_ms = (v3deep_attributes_start - v3deep_deform_start) * 1000.0;
        v3deep_attributes_ms = (v3deep_normals_start - v3deep_attributes_start) * 1000.0;
        v3deep_normals_ms = (end - v3deep_normals_start) * 1000.0;
      }
      sculpt_mesh_v3_clay_setup_ms =
          (BLI_time_now_seconds() - sculpt_mesh_v3_clay_setup_start) * 1000.0;
      const double sculpt_mesh_v3_clay_loop_start = BLI_time_now_seconds();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_faces(depsgraph,
                       sd,
                       brush,
                       mat,
                       offset,
                       vert_normals,
                       attribute_data,
                       nodes[i],
                       object,
                       tls,
                       position_data);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      sculpt_mesh_v3_clay_loop_ms =
          (BLI_time_now_seconds() - sculpt_mesh_v3_clay_loop_start) * 1000.0;      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_grids(depsgraph, sd, object, brush, mat, offset, nodes[i], tls);
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
            calc_bmesh(depsgraph, sd, object, brush, mat, offset, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  const double sculpt_mesh_v3_clay_pbvh_start = BLI_time_now_seconds();
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
  if (sculpt_mesh_v3_clay_mesh_path) {
    const double sculpt_mesh_v3_clay_pbvh_ms =
        (BLI_time_now_seconds() - sculpt_mesh_v3_clay_pbvh_start) * 1000.0;
    const double sculpt_mesh_v3_clay_total_ms =
        (BLI_time_now_seconds() - sculpt_mesh_v3_clay_total_start) * 1000.0;
    if (sculpt_mesh_v3_clay_total_ms >= 0.50) {
      std::fprintf(stderr,
                   "[SculptMeshV3Clay] nodes=%lld total=%.3f ms setup=%.3f "
                   "node_loop=%.3f pbvh_tag_flush=%.3f\n",
                   static_cast<long long>(node_mask.size()),
                   sculpt_mesh_v3_clay_total_ms,
                   sculpt_mesh_v3_clay_setup_ms,
                   sculpt_mesh_v3_clay_loop_ms,
                   sculpt_mesh_v3_clay_pbvh_ms);
    }
  }
  if (v3deep && sculpt_mesh_v3_clay_mesh_path && sculpt_mesh_v3_clay_setup_ms >= 0.50) {
    std::fprintf(stderr,
                 "[SculptMeshV3Setup] nodes=%lld deform=%.3f ms attributes=%.3f normals_access=%.3f\n",
                 static_cast<long long>(node_mask.size()),
                 v3deep_deform_ms, v3deep_attributes_ms, v3deep_normals_ms);
  }
}

namespace clay_strips {

/**
 * Checks whether the node's bounding box overlaps with the region affected by the brush.
 * Clay Strips affects only vertices below the brush plane. The brush-local coordinate
 * system is oriented so that vertices below the plane have positive local z-coordinates.
 * Therefore, we only need to check if the node intersects the [-1,1] x [-1,1] x [0,1] volume in
 * local space.
 */
static bool node_in_box(const float4x4 &mat, const Bounds<float3> &bounds)
{
  const float3 brush_center = float3(0.0f, 0.0f, 0.5f);
  const float3 node_center = math::transform_point(mat, (bounds.max + bounds.min) * 0.5f);
  const float3 center_diff = brush_center - node_center;

  const float3 brush_half_lengths = float3(1.0f, 1.0f, 0.5f);
  const float3 node_half_lengths = (bounds.max - bounds.min) * 0.5f;

  const float3 &node_x_axis = mat.x_axis();
  const float3 &node_y_axis = mat.y_axis();
  const float3 &node_z_axis = mat.z_axis();

  /* Tests if `axis` separates the boxes. */
  auto axis_separates_boxes = [&](const float3 &axis) {
    const float radius1 = math::dot(math::abs(axis), brush_half_lengths);
    const float radius2 = math::abs(math::dot(axis, node_x_axis)) * node_half_lengths.x +
                          math::abs(math::dot(axis, node_y_axis)) * node_half_lengths.y +
                          math::abs(math::dot(axis, node_z_axis)) * node_half_lengths.z;

    const float projection = math::abs(math::dot(center_diff, axis));

    return projection > radius1 + radius2;
  };

  const std::array<float3, 3> brush_axes = {
      float3{1.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f}, float3{0.0f, 0.0f, 1.0f}};
  const std::array<float3, 3> node_axes = {node_x_axis, node_y_axis, node_z_axis};

  /**
   * Intersection is tested using the Separating Axis Theorem.
   * Two boxes (not necessarily axis-aligned) intersect if and only if there does not exist an axis
   * that separates them. In particular, it is necessary and sufficient to:
   */

  /* 1. Test axes aligned with the region affected by the brush. */
  for (const float3 &axis : brush_axes) {
    if (axis_separates_boxes(axis)) {
      return false;
    }
  }

  /* 2. Test axes aligned with the node bounds. */
  for (const float3 &axis : node_axes) {
    if (axis_separates_boxes(axis)) {
      return false;
    }
  }

  /* 3. Test all their cross products. */
  for (const float3 &brush_axis : brush_axes) {
    for (const float3 &node_axis : node_axes) {
      if (axis_separates_boxes(math::cross(brush_axis, node_axis))) {
        return false;
      }
    }
  }

  /* None of the axes separates the boxes: they intersect. */
  return true;
}

float4x4 calc_local_matrix(const Brush &brush,
                           const StrokeCache &cache,
                           const float3 &plane_normal,
                           const float3 &plane_center,
                           const bool flip)
{
  float4x4 mat = float4x4::identity();
  mat.x_axis() = math::cross(plane_normal, cache.grab_delta_symm);
  if (math::is_zero(float3(mat.x_axis()))) {
    // A stationary experimental stamp has no gesture orientation.
    // Choose a deterministic nonparallel axis; legacy callers still
    // reject zero-delta dabs before invoking this function.
    const float3 reference=std::abs(plane_normal.z)<0.9f ?
                               float3(0,0,1) : float3(0,1,0);
    mat.x_axis()=math::cross(plane_normal,reference);
  }
  mat.y_axis() = math::cross(plane_normal, mat.x_axis());
  mat.z_axis() = plane_normal;

  /* Flip the z-axis so that the vertices below the plane have positive z-coordinates. When the
   * brush is inverted, the affected z-coordinates are already positive. */
  if (!flip) {
    mat.z_axis() *= -1.0f;
  }

  mat.location() = plane_center;
  mat = math::normalize(mat);

  /* Scale brush local space matrix. */
  const float4x4 scale = math::from_scale<float4x4>(float3(cache.radius));
  float4x4 tmat = mat * scale;
  tmat.y_axis() *= brush.tip_scale_x;
  mat = math::invert(tmat);
  return mat;
}

CursorSampleResult calc_node_mask(const Depsgraph &depsgraph,
                                  Object &object,
                                  const Brush &brush,
                                  IndexMaskMemory &memory)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const SculptSession &ss = *object.runtime->sculpt_session;

  const bool flip = (ss.cache->bstrength < 0.0f);
  const float displace = ss.cache->radius * brush_plane_offset_get(brush, ss) *
                         (flip ? -1.0f : 1.0f);

  /* TODO: Test to see if the sqrt2 extra factor can be removed */
  const float initial_radius_squared = math::square(ss.cache->radius * std::numbers::sqrt2);

  bool use_original = !ss.cache->accum;
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  if(physics_v15::live_surface_v30_brush(object,brush))use_original=false;
#endif
  IndexMask initial_node_mask = gather_nodes(pbvh,
                                                   eBrushFalloffShape(brush.falloff_shape),
                                                   use_original,
                                                   ss.cache->location_symm,
                                                   initial_radius_squared,
                                                   ss.cache->view_normal_symm,
                                                   memory);

  initial_node_mask=layer_detail_v01::extend_shared_region(
      object,brush,initial_node_mask,memory);
  if (!layer_detail_v01::prepare(object, initial_node_mask)) {
    return {IndexMask(), std::nullopt, std::nullopt};
  }
  const auto &detail = layer_detail_v01::state();
  const bool isolated = detail.active;
  struct SamplingLocationScope {
    StrokeCache &cache;
    float3 location, location_symm;
    void restore() { cache.location=location;cache.location_symm=location_symm; }
    ~SamplingLocationScope() { restore(); }
  } sampling_location{*ss.cache,ss.cache->location,ss.cache->location_symm};
  if (layer_detail_v01::sampling_offset_needed(brush)) {
    const float3 offset=layer_detail_v01::anchor_offset(ss.cache->location_symm);
    ss.cache->location_symm-=offset;
    if (ss.cache->mirror_symmetry_pass==0 && ss.cache->radial_symmetry_pass==0) {
      ss.cache->location-=offset;
    }
  }
  float3 plane_center;
  float3 plane_normal;
  calc_brush_plane(depsgraph, brush, object, initial_node_mask, plane_normal, plane_center);
  plane_normal = tilt_apply_to_normal(plane_normal, *ss.cache, brush.tilt_strength_factor);
  plane_center += plane_normal * ss.cache->scale * displace;

  bool independent_contact=false;
#ifdef WITH_SCULPT_XPBD_OTHER_BRUSHES_V15
  independent_contact=brush.sculpt_physics_mode==3 &&
      ((physics_v15::shape_v24_requested() &&
        brush.sculpt_physics_clay_behavior>=2 && brush.sculpt_physics_clay_behavior<=14 &&
        brush.sculpt_physics_clay_behavior!=4) ||
       (brush.sculpt_physics_clay_behavior>=7 && brush.sculpt_physics_clay_behavior<=14) ||
       (physics_v15::drag_contact_requested() &&
        (brush.sculpt_physics_clay_behavior==2 || brush.sculpt_physics_clay_behavior==5))) &&
      pbvh.type()==bke::pbvh::Type::Mesh && !isolated;
#endif
  if ((!independent_contact && math::is_zero(ss.cache->grab_delta_symm)) ||
      math::is_zero(plane_normal)) {
    /* The brush local matrix is degenerate: return an empty index mask. */
    return {IndexMask(), plane_center, plane_normal};
  }

  const float4x4 mat = calc_local_matrix(brush, *ss.cache, plane_normal, plane_center, flip);

  sampling_location.restore();
  if (independent_contact) {
    // Native plane-box depth culling would discard pressure contact dabs.
    // Retain conservative cursor nodes; exact XY, mask and alpha follow.
    return {initial_node_mask,plane_center,plane_normal};
  }
  if (isolated) {
    /* Candidate bounds belong to the composed surface, while the plane is private.
     * Keep the conservative cursor region; per-vertex brush factors clip the footprint. */
    return {initial_node_mask, plane_center, plane_normal};
  }
  const IndexMask plane_mask = bke::pbvh::search_nodes(
      pbvh, memory, [&](const bke::pbvh::Node &node) {
        if (node_fully_masked_or_hidden(node)) {
          return false;
        }
        return node_in_box(mat, node.bounds());
      });

  return {plane_mask, plane_center, plane_normal};
}
}  // namespace clay_strips

}  // namespace blender::ed::sculpt_paint::brushes
