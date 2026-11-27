/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "physical_material_finish.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"

#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_array.hh"
#include "BLI_math_vector.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#ifdef WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL
#  include "brush_solver_backend.hh"
#endif

namespace blender::ed::sculpt_paint::brushes {

bool apply_physical_lesion_preview(const Depsgraph &depsgraph,
                                   const Sculpt &sd,
                                   const Brush &brush,
                                   Object &object,
                                   const IndexMask &node_mask)
{
#ifdef BSE_VOLUMETRIC_ONLY
  (void)depsgraph; (void)sd; (void)brush; (void)object; (void)node_mask;
  return false;
#else
  const char *enabled = std::getenv("BLENDER_SCULPT_LESION_PREVIEW");
  if (!enabled || enabled[0] != '1') {
    return false;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (pbvh.type() != bke::pbvh::Type::Mesh || cache.radius <= 1.0e-8f) {
    return false;
  }
  float3 stroke_direction = cache.grab_delta_symm;
  if (math::length_squared(stroke_direction) <= 1.0e-12f) {
    return false;
  }
  stroke_direction = math::normalize(stroke_direction);
  const float3 surface_normal = tilt_effective_normal_get(ss, brush);
  float3 opening_direction = math::cross(surface_normal, stroke_direction);
  if (math::length_squared(opening_direction) <= 1.0e-12f) {
    return false;
  }
  opening_direction = math::normalize(opening_direction);

  float opening = 0.35f;
  if (const char *value = std::getenv("BLENDER_SCULPT_LESION_OPENING")) {
    char *end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end != value && std::isfinite(parsed)) {
      opening = std::clamp(parsed, 0.0f, 1.0f);
    }
  }
  if (opening <= 0.0f) {
    return true;
  }

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const PositionDeformData position_data(depsgraph, object, true);
  const MeshAttributeData attributes(mesh);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
  const float amplitude = cache.radius * std::abs(cache.bstrength) * opening * 0.08f;
  node_mask.foreach_index([&](const int node_i) {
    bke::pbvh::MeshNode &node = nodes[node_i];
    const Span<int> verts = node.verts();
    calc_factors_common_mesh_indexed(depsgraph,
                                     brush,
                                     object,
                                     attributes,
                                     position_data.eval,
                                     normals,
                                     node,
                                     factors,
                                     distances);
    translations.resize(verts.size());
    for (const int i : verts.index_range()) {
      const float side = math::dot(position_data.eval[verts[i]] - cache.location_symm,
                                   opening_direction);
      const float center = std::clamp(
          1.0f - std::abs(side) / (cache.radius * 0.45f), 0.0f, 1.0f);
      const float sign = side < 0.0f ? -1.0f : 1.0f;
      translations[i] = opening_direction * (sign * amplitude * center * factors[i]);
    }
    clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
    position_data.deform(translations, verts);
    bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
  });
  return true;
#endif
}

bool apply_physical_material_finish(const Depsgraph &depsgraph,
                                    const Sculpt &sd,
                                    const Brush &brush,
                                    Object &object,
                                    const IndexMask &node_mask)
{
#if defined(BSE_VOLUMETRIC_ONLY) || !defined(WITH_SCULPT_XPBD_GRAB_EXPERIMENTAL)
  (void)depsgraph; (void)sd; (void)brush; (void)object; (void)node_mask;
  return false;
#else
  const char *enabled = std::getenv("BLENDER_SCULPT_PHYSICAL_CLAY_V01");
  const bool brush_material = brush.sculpt_physics_mode == 3;
  if (!brush_material && (!enabled || enabled[0] != '1')) {
    return false;
  }
  int material_id = std::clamp(brush.sculpt_physics_material, 0, 8);
  if (!brush_material) {
    if (const char *material = std::getenv("BLENDER_SCULPT_PHYSICAL_CLAY_MATERIAL")) {
      material_id = STREQ(material, "mud") ? 2 : STREQ(material, "wax") ? 3 : 0;
    }
  }
  static constexpr float weights[] = {.38f, .22f, .62f, .26f, .32f, .48f, .08f, .14f, .44f};
  const float material_weight = weights[material_id];
  const sculpt_physics::SmoothSolverMode mode =
      material_id == 2 || material_id == 8 ? sculpt_physics::SmoothSolverMode::DenseStrong :
      material_id == 5 ? sculpt_physics::SmoothSolverMode::Relax :
                         sculpt_physics::SmoothSolverMode::VolumePreserve;
  SculptSession &ss = *object.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (pbvh.type() != bke::pbvh::Type::Mesh || ss.deform_modifiers_active ||
      ss.shapekey_active || cache.tile_pass != 0 || cache.radial_symmetry_pass != 0 ||
      int(cache.mirror_symmetry_pass) != 0)
  {
    return false;
  }
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const PositionDeformData position_data(depsgraph, object, true);
  if (position_data.eval.size() != mesh.verts_num || mesh.verts_num == 0) {
    return false;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const MeshAttributeData attributes(mesh);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, object);

  try {
    bool created_session = false;
    auto session = std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(
        cache.physical_solver_session);
    if (!session) {
      session = std::static_pointer_cast<sculpt_physics::StrokeSolverSession>(
          ss.physical_solver_session_cache);
    }
    if (!session || session->purpose != sculpt_physics::StrokeSolverPurpose::PhysicalClay ||
        session->vertex_count != std::size_t(mesh.verts_num) ||
        session->selected_backend != sculpt_physics::BackendKind::WarpCuda)
    {
      session = sculpt_physics::create_stroke_solver_session(
          sculpt_physics::StrokeSolverPurpose::PhysicalClay,
          sculpt_physics::BackendKind::WarpCuda,
          std::size_t(mesh.verts_num));
      if (session->selected_backend != sculpt_physics::BackendKind::WarpCuda) {
        return false;
      }
      for (const int vertex : position_data.eval.index_range()) {
        const float3 p = position_data.eval[vertex];
        const bool hidden = !attributes.hide_vert.is_empty() && attributes.hide_vert[vertex];
        const float mask = attributes.mask.is_empty() ? 0.0f : attributes.mask[vertex];
        session->solver->add_particle({double(p.x), double(p.y), double(p.z)},
                                      hidden || mask >= 0.999f ? 0.0 : 1.0 - double(mask));
      }
      for (const int2 edge : mesh.edges()) {
        session->solver->add_bond(std::size_t(edge[0]), std::size_t(edge[1]), 1.0e-4);
      }
      session->solver->set_shape_stiffness_override(0.0);
      session->solver->begin_stroke();
      ss.physical_solver_session_cache = session;
      created_session = true;
    }
    cache.physical_solver_session = session;

    std::vector<std::size_t> &active = session->compact_indices;
    std::vector<float> &weights = session->compact_weights;
    std::vector<sculpt_physics::Vec3> &active_normals = session->compact_normals;
    active.clear();
    weights.clear();
    active_normals.clear();
    if (++session->compact_generation == 0) {
      std::fill(session->compact_stamp.begin(), session->compact_stamp.end(), 0);
      session->compact_generation = 1;
    }
    const std::uint32_t generation = session->compact_generation;
    std::vector<int> &active_slot = session->compact_slot;
    std::vector<std::uint32_t> &active_stamp = session->compact_stamp;
    Vector<float> factors;
    Vector<float> distances;
    node_mask.foreach_index([&](const int node_i) {
      const bke::pbvh::MeshNode &node = nodes[node_i];
      const Span<int> verts = node.verts();
      calc_factors_common_mesh_indexed(depsgraph,
                                       brush,
                                       object,
                                       attributes,
                                       position_data.eval,
                                       normals,
                                       node,
                                       factors,
                                       distances);
      for (const int i : verts.index_range()) {
        if (factors[i] <= 0.0f || !std::isfinite(factors[i])) {
          continue;
        }
        const float weight = std::clamp(factors[i] * material_weight, 0.0f, 1.0f);
        if (active_stamp[verts[i]] == generation) {
          weights[std::size_t(active_slot[verts[i]])] = std::max(
              weights[std::size_t(active_slot[verts[i]])], weight);
          continue;
        }
        active_stamp[verts[i]] = generation;
        active_slot[verts[i]] = int(active.size());
        active.push_back(std::size_t(verts[i]));
        weights.push_back(weight);
        const float3 n = normals[verts[i]];
        active_normals.push_back({double(n.x), double(n.y), double(n.z)});
      }
    });
    if (active.empty()) {
      return true;
    }
    if (!created_session) {
      std::vector<sculpt_physics::Vec3> &current = session->compact_positions;
      current.clear();
      current.reserve(active.size());
      for (const std::size_t vertex : active) {
        const float3 p = position_data.eval[int(vertex)];
        current.push_back({double(p.x), double(p.y), double(p.z)});
      }
      if (!session->solver->synchronize_positions_compact(active, current, false)) {
        return false;
      }
    }
    if (!session->solver->smooth_compact(
            active, weights, active_normals, mode, double(cache.radius)))
    {
      return false;
    }

    Vector<float3> translations;
    node_mask.foreach_index([&](const int node_i) {
      bke::pbvh::MeshNode &node = nodes[node_i];
      const Span<int> verts = node.verts();
      translations.resize(verts.size());
      for (const int i : verts.index_range()) {
        if (active_stamp[verts[i]] != generation) {
          translations[i] = float3(0.0f);
          continue;
        }
        const sculpt_physics::Vec3 p = session->solver->particles()[verts[i]].position;
        translations[i] = float3(float(p.x), float(p.y), float(p.z)) - position_data.eval[verts[i]];
      }
      clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
      position_data.deform(translations, verts);
      bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
    });
    /* Every caller owns the brush transaction and tags/flushed these nodes
     * immediately after this optional finish. Doing it here as well doubled
     * PBVH propagation cost for every physical dab. */
    return true;
  }
  catch (...) {
    cache.physical_solver_session.reset();
    ss.physical_solver_session_cache.reset();
    return false;
  }
#endif
}

}  // namespace blender::ed::sculpt_paint::brushes
