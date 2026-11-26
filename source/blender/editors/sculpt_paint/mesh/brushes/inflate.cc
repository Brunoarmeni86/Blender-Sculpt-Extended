/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace inflate_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

static float environment_unit(const char *name, const float fallback, const float maximum = 1.0f)
{
  const char *value = std::getenv(name);
  if (!value) {
    return fallback;
  }
  char *end = nullptr;
  const float parsed = std::strtof(value, &end);
  return end != value && std::isfinite(parsed) ? std::clamp(parsed, 0.0f, maximum) : fallback;
}

/* A balloon deforms with a coherent scalar pressure field. The displacement
 * direction is a low-frequency normal field, while positions are never
 * smoothed. This transports pores and Multires-like detail without amplifying
 * per-vertex normal noise into faceting. */
static bool apply_physical_balloon_mesh(const Depsgraph &depsgraph,
                                        const Sculpt &sd,
                                        const Brush &brush,
                                        Object &object,
                                        const IndexMask &node_mask,
                                        const float3 &base_scale,
                                        bke::pbvh::Tree &pbvh)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_PHYSICAL_BALLOON");
  if (!enabled || enabled[0] != '1') {
    return false;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  if (ss.vert_to_edge_map.is_empty() && !mesh.edges().is_empty()) {
    ss.vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
        mesh.edges(), mesh.verts_num, ss.vert_to_edge_offsets, ss.vert_to_edge_indices);
  }
  const PositionDeformData position_data(depsgraph, object, true);
  const MeshAttributeData attributes(mesh);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const float pressure = environment_unit("BLENDER_SCULPT_BALLOON_PRESSURE", 1.0f, 2.0f);
  const float tension = environment_unit("BLENDER_SCULPT_BALLOON_TENSION", 0.65f);

  std::vector<int> active;
  std::vector<int> slot(std::size_t(mesh.verts_num), -1);
  std::vector<float> field;
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
      if (factors[i] <= 0.0f) {
        continue;
      }
      if (slot[verts[i]] >= 0) {
        field[std::size_t(slot[verts[i]])] = std::max(
            field[std::size_t(slot[verts[i]])], factors[i]);
        continue;
      }
      slot[verts[i]] = int(active.size());
      active.push_back(verts[i]);
      field.push_back(factors[i]);
    }
  });
  if (active.empty()) {
    return true;
  }

  if (!ss.vert_to_edge_map.is_empty()) {
    std::vector<float> next(field.size());
    const Span<int2> edges = mesh.edges();
    const int iterations = 1 + int(std::round(tension * 7.0f));
    const float relaxation = 0.25f + tension * 0.65f;
    for (int iteration = 0; iteration < iterations; iteration++) {
      for (std::size_t i = 0; i < active.size(); i++) {
        float sum = field[i];
        int count = 1;
        for (const int edge_i : ss.vert_to_edge_map[active[i]]) {
          const int2 edge = edges[edge_i];
          const int other = edge[0] == active[i] ? edge[1] : edge[0];
          if (slot[other] >= 0) {
            sum += field[std::size_t(slot[other])];
            count++;
          }
        }
        const float average = sum / float(count);
        next[i] = math::interpolate(field[i], average, relaxation);
      }
      field.swap(next);
    }
  }

  /* Filter only the pressure direction, not the surface positions. Include
   * inactive one-ring normals as a fixed collar so the active region cannot
   * form a visible rim at the brush boundary. */
  std::vector<float3> directions(active.size());
  std::vector<float3> next_directions(active.size());
  for (std::size_t i = 0; i < active.size(); i++) {
    directions[i] = normals[active[i]];
  }
  if (!ss.vert_to_edge_map.is_empty()) {
    const Span<int2> edges = mesh.edges();
    /* Estimate topology scale from a bounded sample. A fixed three-ring
     * normal filter was sufficient on medium meshes but exposed individual
     * facets once many more vertices fitted inside the same brush radius. */
    double sampled_edge_length = 0.0;
    int sampled_edges = 0;
    for (std::size_t i = 0; i < active.size() && sampled_edges < 4096; i++) {
      const int vertex = active[i];
      for (const int edge_i : ss.vert_to_edge_map[vertex]) {
        const int2 edge = edges[edge_i];
        const int other = edge[0] == vertex ? edge[1] : edge[0];
        if (other <= vertex) {
          continue;
        }
        const float length = math::distance(position_data.eval[vertex], position_data.eval[other]);
        if (length > 1.0e-12f && std::isfinite(length)) {
          sampled_edge_length += double(length);
          sampled_edges++;
        }
        if (sampled_edges >= 4096) {
          break;
        }
      }
    }
    const double mean_edge = sampled_edges > 0 ?
                                 sampled_edge_length / double(sampled_edges) :
                                 double(ss.cache->radius);
    const double samples_across_radius = std::max(
        1.0, double(ss.cache->radius) / std::max(mean_edge, 1.0e-12));
    const int density_passes = int(std::ceil(std::log2(samples_across_radius))) + 1;
    const int direction_iterations = std::clamp(
        density_passes + int(std::round(tension * 2.0f)), 4, 12);
    const float direction_relaxation = 0.55f + tension * 0.30f;
    for (int iteration = 0; iteration < direction_iterations; iteration++) {
      for (std::size_t i = 0; i < active.size(); i++) {
        float3 sum = directions[i] * 2.0f;
        int weight = 2;
        for (const int edge_i : ss.vert_to_edge_map[active[i]]) {
          const int2 edge = edges[edge_i];
          const int other = edge[0] == active[i] ? edge[1] : edge[0];
          const int other_slot = slot[other];
          sum += other_slot >= 0 ? directions[std::size_t(other_slot)] : normals[other];
          weight++;
        }
        const float3 average = math::normalize(sum / float(weight));
        next_directions[i] = math::normalize(
            math::interpolate(directions[i], average, direction_relaxation));
      }
      directions.swap(next_directions);
    }

    /* Filter the scalar pressure as well as its direction. Brush texture and
     * high-frequency factor variation otherwise survive the normal filter and
     * create a different displacement length at every dense-mesh vertex. The
     * inactive one-ring is a fixed zero-pressure collar. */
    const int extra_field_iterations = std::clamp(direction_iterations / 2, 2, 6);
    std::vector<float> filtered_field(field.size());
    for (int iteration = 0; iteration < extra_field_iterations; iteration++) {
      for (std::size_t i = 0; i < active.size(); i++) {
        float sum = field[i] * 2.0f;
        int weight = 2;
        for (const int edge_i : ss.vert_to_edge_map[active[i]]) {
          const int2 edge = edges[edge_i];
          const int other = edge[0] == active[i] ? edge[1] : edge[0];
          const int other_slot = slot[other];
          sum += other_slot >= 0 ? field[std::size_t(other_slot)] : 0.0f;
          weight++;
        }
        filtered_field[i] = math::interpolate(field[i], sum / float(weight), 0.65f);
      }
      field.swap(filtered_field);
    }
  }

  /* Balloon pressure must be coherent at the center of the dab. Blend only
   * the displacement direction toward the brush surface normal; keep the
   * original positions untouched so pores and high-frequency detail are
   * transported rather than erased. The blend fades with brush influence,
   * preserving continuity at the boundary collar. */
  float3 pressure_normal = tilt_effective_normal_get(ss, brush);
  if (math::length_squared(pressure_normal) > 1.0e-12f) {
    pressure_normal = math::normalize(pressure_normal);
    const float coherence = 0.90f + tension * 0.10f;
    for (std::size_t i = 0; i < active.size(); i++) {
      float3 direction = directions[i];
      if (math::dot(direction, pressure_normal) < 0.0f) {
        direction = -direction;
      }
      const float center_weight = std::clamp(field[i] * 2.0f, 0.0f, 1.0f);
      const float blend = coherence * center_weight;
      const float3 coherent = math::interpolate(direction, pressure_normal, blend);
      directions[i] = math::length_squared(coherent) > 1.0e-12f ?
                          math::normalize(coherent) :
                          pressure_normal;
    }
  }

  Vector<float3> translations;
  node_mask.foreach_index([&](const int node_i) {
    bke::pbvh::MeshNode &node = nodes[node_i];
    const Span<int> verts = node.verts();
    translations.resize(verts.size());
    for (const int i : verts.index_range()) {
      const int index = slot[verts[i]];
      translations[i] = index >= 0 ? directions[std::size_t(index)] * base_scale *
                                         (field[std::size_t(index)] * pressure) :
                                     float3(0.0f);
    }
    clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
    position_data.deform(translations, verts);
    bke::pbvh::update_node_bounds_mesh(position_data.eval, node);
  });
  return true;
}

BLI_NOINLINE static void apply_scale(const MutableSpan<float3> translations, const float3 &scale)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : translations.index_range()) {
    translations[i] *= scale;
  }
}

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float3 &scale,
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

  const MutableSpan translations = gather_data_mesh(vert_normals, verts, tls.translations);
  apply_scale(translations, scale);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float3 &scale,
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
  gather_grids_normals(subdiv_ccg, grids, translations);
  apply_scale(translations, scale);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float3 &scale,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  gather_bmesh_normals(verts, translations);
  apply_scale(translations, scale);
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace inflate_cc

void do_inflate_brush(const Depsgraph &depsgraph,
                      const Sculpt &sd,
                      Object &object,
                      const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  const float3 scale = ss.cache->scale * ss.cache->radius * ss.cache->bstrength;

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      if (apply_physical_balloon_mesh(depsgraph, sd, brush, object, node_mask, scale, pbvh)) {
        break;
      }
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
                       scale,
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
            calc_grids(depsgraph, sd, object, brush, scale, nodes[i], tls);
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
            calc_bmesh(depsgraph, sd, object, brush, scale, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

}  // namespace blender::ed::sculpt_paint::brushes
