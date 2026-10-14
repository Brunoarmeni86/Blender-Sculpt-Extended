/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cstring>
#include <cmath>
#include <cstdio>

#include "BKE_multires_sculpt_layers.hh"
#include "polypaint_cpu_math.hh"
#include "BLI_color.hh"
#include "multires_layer_base_math.hh"
#include "multires_layer_corner_identity.hh"
#include "BKE_attribute.hh"
#include "BKE_idprop.hh"
#include "BKE_subdiv.hh"
#include "BKE_customdata.hh"
#include "BKE_key.hh"
#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_multires.hh"
#include "BKE_modifier.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "DEG_depsgraph_query.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_shrinkwrap.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subdiv_eval.hh"
#ifdef WITH_OPENSUBDIV
#  include "opensubdiv_evaluator.hh"
#endif
#include "multires_reshape.hh"
#include "DNA_mesh_types.h"
#include "DNA_key_types.h"
#include "DNA_object_types.h"

#include "DNA_modifier_types.h"
#include "DNA_meshdata_types.h"

#include <mutex>

#include "BLI_listbase.h"
#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_bitmap.h"
#include "BLI_string.h"
#include "BLI_string_utils.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_base.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_path_utils.hh"
#include "BLI_task.hh"
#include "BLI_time.h"

#include "DEG_depsgraph_query.hh"

#include "MEM_guardedalloc.h"

namespace blender::bke::multires_layers {

struct LayerPayloadIndex {
  const MultiresSculptLayer *layer = nullptr;
  const MDisps *descriptors = nullptr;
  int grids_num = 0;
  int level = 0;
  Vector<int> grids;

  void clear()
  {
    layer = nullptr;
    descriptors = nullptr;
    grids_num = level = 0;
    grids.clear();
  }
};

static LayerPayloadIndex &payload_index()
{
  static LayerPayloadIndex cache;
  return cache;
}

static std::mutex &payload_index_mutex()
{
  static std::mutex mutex;
  return mutex;
}

void payload_index_invalidate(const MultiresSculptLayer *layer)
{
  std::lock_guard<std::mutex> lock(payload_index_mutex());
  LayerPayloadIndex &cache = payload_index();
  if (layer == nullptr || cache.layer == layer) {
    cache.clear();
  }
}

bool payload_grids(const MultiresSculptLayer &layer,
                   const int expected_grids,
                   Vector<int> &r_grids)
{
  r_grids.clear();
  if (layer.disps == nullptr || layer.grids_num != expected_grids ||
      layer.level <= 0 || layer.level > 13)
  {
    return false;
  }
  const int side = subdiv::grid_size_from_level(layer.level);
  const int area = side * side;
  const char *value = BLI_getenv("BLENDER_SCULPT_LAYER_SUPPORT_CACHE");
  const bool enabled = value != nullptr && value[0] == '1' && value[1] == '\0';
  std::lock_guard<std::mutex> lock(payload_index_mutex());
  LayerPayloadIndex &cache = payload_index();
  const bool hit = enabled && cache.layer == &layer && cache.descriptors == layer.disps &&
                   cache.grids_num == expected_grids && cache.level == layer.level;
  if (hit) {
    /* Validate occupied descriptors; zero-payload allocation changes invalidate
     * the index via grid_ensure before the next property transaction. */
    for (const int grid : cache.grids) {
      const MDisps &data = layer.disps[grid];
      if (data.disps == nullptr || data.level != layer.level || data.totdisp != area) {
        cache.clear();
        return false;
      }
    }
    r_grids.extend(cache.grids.as_span());
  }
  else {
    cache.clear();
    for (int grid = 0; grid < expected_grids; grid++) {
      const MDisps &data = layer.disps[grid];
      if (data.disps == nullptr) {
        continue;
      }
      if (data.level != layer.level || data.totdisp != area) {
        return false;
      }
      r_grids.append(grid);
    }
    if (enabled) {
      cache.layer = &layer;
      cache.descriptors = layer.disps;
      cache.grids_num = expected_grids;
      cache.level = layer.level;
      cache.grids.extend(r_grids.as_span());
    }
  }
  if (BLI_getenv("BLENDER_LAYER_RESP_TRACE")) {
    std::printf("[LAYER_SUPPORT_INDEX] mode=%s grids=%d/%d\n",
                enabled ? (hit ? "HIT" : "BUILD") : "OFF",
                int(r_grids.size()), expected_grids);
  }
  return true;
}

bool is_effective(const MultiresSculptLayer &layer)
{
  return (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0 && std::isfinite(layer.strength) &&
         std::abs(layer.strength) >= 1e-6f;
}

bool base_has_visible_layers(const MultiresModifierData &mmd)
{
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) {
      return true;
    }
  }
  return false;
}

MultiresModifierData *base_modifier(Object &object)
{
  for (ModifierData &md : object.modifiers) {
    if (md.type == eModifierType_Multires && (md.mode & eModifierMode_Realtime)) {
      auto *mmd = reinterpret_cast<MultiresModifierData *>(&md);
      if (mmd->sculptlvl == 0 || (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
        return mmd;
      }
      return nullptr;
    }
  }
  return nullptr;
}

MultiresSculptLayer *base_recording_layer(Object &object)
{
  MultiresModifierData *mmd = base_modifier(object);
  if (!mmd || !mmd->sculpt_layer_recording ||
      (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
    return nullptr;
  }
  Mesh *mesh = id_cast<Mesh *>(object.data);
  if (!can_record(*mmd, *mesh)) {
    return nullptr;
  }
  return active(*mmd);
}

void base_ensure_vertex_ids(Mesh &mesh)
{
  if (mesh.attributes().contains(vertex_id_attribute)) {
    return;
  }
  SpanAttributeWriter<int> ids = mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
      vertex_id_attribute, AttrDomain::Point);
  for (const int vertex : ids.span.index_range()) {
    ids.span[vertex] = vertex + 1;
  }
  ids.finish();
}

bool base_remap_after_topology_edit(MultiresModifierData &mmd, Mesh &mesh)
{
  int stored_count = -1;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.base_deltas || layer.base_verts_num > 0) {
      if (stored_count >= 0 && stored_count != layer.base_verts_num) {
        return false;
      }
      stored_count = layer.base_verts_num;
    }
  }
  if (stored_count == -1) {
    /* No cage deltas depend on the old IDs. Reindex even if the attribute survived an edit,
     * so later creation of L0 deltas starts from the current vertex order. */
    mesh.attributes_for_write().remove(vertex_id_attribute);
    base_ensure_vertex_ids(mesh);
    return true;
  }
  const AttributeReader<int> ids_reader = mesh.attributes().lookup<int>(vertex_id_attribute,
                                                                       AttrDomain::Point);
  if (!ids_reader) {
    return false;
  }
  const VArraySpan<int> ids(ids_reader.varray);
  bool identity = stored_count == mesh.verts_num;
  if (identity) {
    for (const int vertex : ids.index_range()) {
      identity &= ids[vertex] == vertex + 1;
    }
  }
  if (identity) {
    return true;
  }
  Array<int> counts(stored_count, 0);
  for (const int stored_id : ids) {
    const int id = stored_id - 1;
    if (id >= 0 && id < stored_count) {
      counts[id]++;
    }
  }
  Array<int> map(mesh.verts_num, -1);
  for (const int vertex : ids.index_range()) {
    const int id = ids[vertex] - 1;
    if (id >= 0 && id < stored_count && counts[id] == 1) {
      map[vertex] = id;
    }
  }
  if (!base_deltas_remap(mmd, stored_count, map)) {
    return false;
  }
  mesh.attributes_for_write().remove(vertex_id_attribute);
  base_ensure_vertex_ids(mesh);
  return true;
}

bool base_deltas_ensure(MultiresSculptLayer &layer, const int verts_num)
{
  if (verts_num < 0 || (layer.base_verts_num != 0 && layer.base_verts_num != verts_num)) {
    return false;
  }
  if (!layer.base_deltas && verts_num > 0) {
    layer.base_deltas = MEM_new_array_zeroed<float[3]>(verts_num, "sculpt layer cage deltas");
  }
  layer.base_verts_num = verts_num;
  return true;
}

bool base_deltas_apply(const MultiresModifierData &mmd, const MutableSpan<float3> positions)
{
  std::vector<base_math::LayerView> layers;
  layers.reserve(mmd.sculpt_layers.count());
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if ((!layer.base_deltas && layer.base_verts_num == 0) || !is_effective(layer)) {
      continue;
    }
    layers.push_back({layer.base_deltas, layer.base_verts_num, layer.strength, true});
  }
  if (layers.empty()) {
    return true;
  }
  return base_math::compose(reinterpret_cast<const float (*)[3]>(positions.data()),
                            int(positions.size()),
                            layers,
                            reinterpret_cast<float (*)[3]>(positions.data()));
}

bool base_deltas_record(MultiresSculptLayer &layer,
                        const Span<int> vertices,
                        const Span<float3> translations)
{
  if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) || vertices.size() != translations.size()) {
    return false;
  }
  return base_math::record(layer.base_deltas, layer.base_verts_num, layer.strength,
                            vertices.data(),
                            reinterpret_cast<const float (*)[3]>(translations.data()),
                            int(vertices.size()));
}

bool base_deltas_remap(MultiresModifierData &mmd,
                       const int old_verts_num,
                       const Span<int> new_to_old)
{
  if (old_verts_num < 0 || new_to_old.size() > INT_MAX) {
    return false;
  }
  struct Staged {
    MultiresSculptLayer *layer;
    float (*deltas)[3];
  };
  std::vector<Staged> staged;
  const auto discard = [&]() {
    for (const Staged &item : staged) {
      MEM_delete(item.deltas);
    }
  };
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!layer.base_deltas && layer.base_verts_num == 0) {
      continue;
    }
    if (layer.base_verts_num != old_verts_num) {
      discard();
      return false;
    }
    float (*deltas)[3] = new_to_old.is_empty() ? nullptr :
        MEM_new_array_zeroed<float[3]>(new_to_old.size(), "remapped sculpt layer cage deltas");
    staged.push_back({&layer, deltas});
    if (!base_math::remap(layer.base_deltas, old_verts_num, new_to_old.data(),
                           int(new_to_old.size()), deltas)) {
      discard();
      return false;
    }
  }
  for (const Staged &item : staged) {
    MEM_SAFE_DELETE(item.layer->base_deltas);
    item.layer->base_deltas = item.deltas;
    item.layer->base_verts_num = int(new_to_old.size());
  }
  return true;
}

static ShrinkwrapModifierData shrinkwrap_settings(const MultiresModifierData &mmd)
{
  ShrinkwrapModifierData smd{};
  smd.target = mmd.sculpt_shrinkwrap_target;
  smd.auxTarget = mmd.sculpt_shrinkwrap_aux_target;
  smd.keepDist = mmd.sculpt_shrinkwrap_offset;
  smd.shrinkType = mmd.sculpt_shrinkwrap_method;
  smd.shrinkMode = mmd.sculpt_shrinkwrap_mode;
  smd.shrinkOpts = mmd.sculpt_shrinkwrap_options;
  smd.projAxis = mmd.sculpt_shrinkwrap_axis;
  smd.projLimit = mmd.sculpt_shrinkwrap_project_limit;
  smd.subsurfLevels = mmd.sculpt_shrinkwrap_subsurf_levels;
  return smd;
}

static bool shrinkwrap_enabled(const MultiresModifierData &mmd)
{
  return mmd.sculpt_shrinkwrap_enabled && mmd.sculpt_shrinkwrap_target &&
         mmd.sculpt_shrinkwrap_strength > 0.0f;
}

bool shrinkwrap_mesh(Depsgraph *depsgraph,
                     Object &object,
                     const MultiresModifierData &mmd,
                     Mesh &mesh,
                     const bool respect_mask,
                     const Span<float> sampled_masks)
{
  if (!depsgraph || !shrinkwrap_enabled(mmd) ||
      (!sampled_masks.is_empty() && sampled_masks.size() != mesh.verts_num)) {
    return false;
  }
  ShrinkwrapModifierData smd = shrinkwrap_settings(mmd);
  const int group = BKE_object_defgroup_name_index(&object, mmd.sculpt_shrinkwrap_vgroup);
  const Span<MDeformVert> dverts = mesh.deform_verts();
  const bool invert = smd.shrinkOpts & MOD_SHRINKWRAP_INVERT_VGROUP;
  const VArray<float> masks = sampled_masks.is_empty() ?
      mesh.attributes().lookup_or_default<float>(".sculpt_mask", AttrDomain::Point, 0.0f).varray :
      VArray<float>::from_span(sampled_masks);
  Array<float> weights(mesh.verts_num);
  for (const int i : weights.index_range()) {
    weights[i] = BKE_defvert_array_find_weight_safe(
        dverts.is_empty() ? nullptr : dverts.data(), i, group, invert) *
        std::clamp(mmd.sculpt_shrinkwrap_strength * (1.0f - (respect_mask ? masks[i] : 0.0f)), 0.0f, 1.0f);
  }
  const bool normal_projection = smd.shrinkType == MOD_SHRINKWRAP_PROJECT && smd.projAxis == 0;
  const Span<float3> normals = normal_projection ? mesh.vert_normals() : Span<float3>();
  Array<float3> subdivided;
  if (normal_projection && smd.subsurfLevels > 0) {
    subdivided = shrinkwrap_subdivided_positions(mesh, smd.subsurfLevels);
  }
  const Span<float3> reference = subdivided.is_empty() ? mesh.vert_positions() : subdivided.as_span();
  const ModifierEvalContext ctx{depsgraph, &object, ModifierApplyFlag(0)};
  const bool changed = shrinkwrap_samples_deform(
      ctx, smd, mesh.vert_positions_for_write(), normals, reference, weights);
  if (changed) {
    mesh.tag_positions_changed();
  }
  return changed;
}

/* Match subdivision's vertex-group interpolation, including the original quad parameterization.
 * Inverting after interpolation matches the standard modifier on a subdivided mesh. */
static Array<float> shrinkwrap_grid_interpolate(const Mesh &mesh,
                                              const SubdivCCG &ccg,
                                              const Span<float> coarse_values)
{
  const auto weight = [&](const int vert) { return coarse_values[vert]; };
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  Array<float> weights(ccg.positions.size());
  const int side = ccg.grid_size;
  const float inv = 1.0f / float(side - 1);
  threading::parallel_for(faces.index_range(), 128, [&](const IndexRange range) {
    for (const int face : range) {
      const IndexRange corners = faces[face];
      float center = 0.0f;
      for (const int corner : corners) {
        center += weight(corner_verts[corner]);
      }
      center /= float(corners.size());
      for (const int corner : corners) {
        const int local = corner - corners.start();
        const float current = weight(corner_verts[corner]);
        const float next = weight(corner_verts[corners.start() + (local + 1) % corners.size()]);
        const float prev = weight(corner_verts[corners.start() +
                                                      (local + corners.size() - 1) % corners.size()]);
        for (int y = 0; y < side; y++) {
          for (int x = 0; x < side; x++) {
            float u, v;
            float w;
            if (corners.size() == 4) {
              subdiv::rotate_grid_to_quad(local, float(x) * inv, float(y) * inv, &u, &v);
              w = weight(corner_verts[corners[0]]) * (1-u) * (1-v) +
                  weight(corner_verts[corners[1]]) * u * (1-v) +
                  weight(corner_verts[corners[2]]) * u * v +
                  weight(corner_verts[corners[3]]) * (1-u) * v;
            }
            else {
              subdiv::grid_uv_to_ptex_face_uv(float(x) * inv, float(y) * inv, &u, &v);
              w = current * (1-u) * (1-v) + (current+next) * 0.5f * u * (1-v) +
                  center * u * v + (current+prev) * 0.5f * (1-u) * v;
            }
            const int i = corner * side * side + y * side + x;
            weights[i] = w;
          }
        }
      }
    }
  });
  return weights;
}

static Array<float> shrinkwrap_grid_weights(const Object &object,
                                           const MultiresModifierData &mmd,
                                           const Mesh &mesh,
                                           const SubdivCCG &ccg,
                                           const bool respect_mask)
{
  const int group = BKE_object_defgroup_name_index(&object, mmd.sculpt_shrinkwrap_vgroup);
  const Span<MDeformVert> dverts = mesh.deform_verts();
  const bool invert = mmd.sculpt_shrinkwrap_options & MOD_SHRINKWRAP_INVERT_VGROUP;
  Array<float> coarse(mesh.verts_num);
  for (const int i : coarse.index_range()) {
    coarse[i] = BKE_defvert_array_find_weight_safe(
        dverts.is_empty() ? nullptr : dverts.data(), i, group, invert);
  }
  Array<float> weights = shrinkwrap_grid_interpolate(mesh, ccg, coarse);
  threading::parallel_for(weights.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      const float mask = respect_mask && !ccg.masks.is_empty() ? ccg.masks[i] : 0.0f;
      weights[i] *= std::clamp(mmd.sculpt_shrinkwrap_strength * (1.0f-mask), 0.0f, 1.0f);
    }
  });
  return weights;
}

/* Only normal Project with Subdivision Levels needs source topology. Weld logical grid seams
 * (never by coordinate proximity), then use the normal modifier's subdivision helper. Other
 * modes operate on sample spans and do not allocate a high-resolution mesh. */
static Array<float3> shrinkwrap_grid_projection_positions(const SubdivCCG &ccg,
                                                          const Mesh &base_mesh,
                                                          const int level)
{
  const int side = ccg.grid_size;
  const int area = side * side;
  Array<int> map(ccg.positions.size());
  for (const int i : map.index_range()) {
    map[i] = i;
  }
  SubdivCCGNeighbors neighbors;
  for (int grid = 0; grid < ccg.grids_num; grid++) {
    for (int y = 0; y < side; y++) {
      for (int x = 0; x < side; x++) {
        if (x != 0 && x != side-1 && y != 0 && y != side-1) {
          continue;
        }
        const int i = grid * area + y * side + x;
        BKE_subdiv_ccg_neighbor_coords_get(ccg, {grid, short(x), short(y)}, true, neighbors);
        for (const SubdivCCGCoord &coord : neighbors.duplicates()) {
          map[i] = std::min(map[i], coord.grid_index * area + coord.y * side + coord.x);
        }
      }
    }
  }
  Array<int> compact(map.size(), -1);
  int verts_num = 0;
  for (const int i : map.index_range()) {
    if (map[i] == i) {
      compact[i] = verts_num++;
    }
  }
  const int faces_num = ccg.grids_num * (side-1) * (side-1);
  Mesh *mesh = BKE_mesh_new_nomain(verts_num, 0, faces_num, faces_num * 4);
  Array<int> original_indices(verts_num);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (const int i : map.index_range()) {
    if (map[i] == i) {
      original_indices[compact[i]] = i;
      positions[compact[i]] = ccg.positions[i];
    }
    map[i] = compact[map[i]];
  }
  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corners = mesh->corner_verts_for_write();
  int face = 0;
  for (int grid = 0; grid < ccg.grids_num; grid++) {
    for (int y = 0; y < side-1; y++) {
      for (int x = 0; x < side-1; x++, face++) {
        const int i = grid * area + y * side + x;
        offsets[face] = face * 4;
        corners[face*4+0] = map[i];
        corners[face*4+1] = map[i+side];
        corners[face*4+2] = map[i+side+1];
        corners[face*4+3] = map[i+1];
      }
    }
  }
  offsets.last() = corners.size();
  mesh_calc_edges(*mesh, false, false);
  const AttributeAccessor base_attrs = base_mesh.attributes();
  if (const AttributeReader<float> coarse = base_attrs.lookup<float>("crease_vert", AttrDomain::Point)) {
    Array<float> coarse_values(base_mesh.verts_num);
    coarse.varray.materialize(coarse_values);
    const Array<float> values = shrinkwrap_grid_interpolate(base_mesh, ccg, coarse_values);
    SpanAttributeWriter<float> crease = mesh->attributes_for_write().lookup_or_add_for_write_span<float>(
        "crease_vert", AttrDomain::Point);
    for (const int i : original_indices.index_range()) {
      crease.span[i] = values[original_indices[i]];
    }
    crease.finish();
  }
  if (const AttributeReader<float> coarse = base_attrs.lookup<float>("crease_edge", AttrDomain::Edge)) {
    Map<OrderedEdge, float> coarse_edges;
    for (const int edge : base_mesh.edges().index_range()) {
      const int2 verts = base_mesh.edges()[edge];
      coarse_edges.add(OrderedEdge(verts[0], verts[1]), coarse.varray[edge]);
    }
    const auto adjacency = [&](const int vert, int &v1, int &v2) {
      const int i = original_indices[vert];
      const SubdivCCGCoord coord{i / area, short(i % side), short((i % area) / side)};
      return BKE_subdiv_ccg_coarse_mesh_adjacency_info_get(
          ccg, coord, base_mesh.corner_verts(), base_mesh.faces(), v1, v2);
    };
    SpanAttributeWriter<float> crease = mesh->attributes_for_write().lookup_or_add_for_write_span<float>(
        "crease_edge", AttrDomain::Edge);
    for (const int edge : mesh->edges().index_range()) {
      const int2 verts = mesh->edges()[edge];
      int a1=-1, a2=-1, b1=-1, b2=-1;
      const SubdivCCGAdjacencyType a = adjacency(verts[0], a1, a2);
      const SubdivCCGAdjacencyType b = adjacency(verts[1], b1, b2);
      float value = 0.0f;
      if (a == SubdivCCGAdjacencyType::Edge && b == SubdivCCGAdjacencyType::Edge &&
          OrderedEdge(a1,a2) == OrderedEdge(b1,b2)) {
        value = coarse_edges.lookup_default(OrderedEdge(a1,a2), 0.0f);
      }
      else if (a == SubdivCCGAdjacencyType::Vertex && b == SubdivCCGAdjacencyType::Edge &&
               ELEM(a1,b1,b2)) {
        value = coarse_edges.lookup_default(OrderedEdge(b1,b2), 0.0f);
      }
      else if (b == SubdivCCGAdjacencyType::Vertex && a == SubdivCCGAdjacencyType::Edge &&
               ELEM(b1,a1,a2)) {
        value = coarse_edges.lookup_default(OrderedEdge(a1,a2), 0.0f);
      }
      crease.span[edge] = value;
    }
    crease.finish();
  }
  const Array<float3> smooth = shrinkwrap_subdivided_positions(*mesh, level);
  Array<float3> result(map.size());
  for (const int i : map.index_range()) {
    result[i] = smooth[map[i]];
  }
  BKE_id_free(nullptr, mesh);
  return result;
}

/* The normal modifier projects along angle-weighted mesh vertex normals. Sculpt CCG
 * normals use a different averaging rule, so compute the native mesh rule directly on
 * grids and sum contributions at logical seams. No high-resolution topology is allocated. */
static void shrinkwrap_grid_mesh_normals(SubdivCCG &ccg)
{
  const int side = ccg.grid_size;
  const int area = side * side;
  Array<float3> sums(ccg.positions.size(), float3(0.0f));
  threading::parallel_for(IndexRange(ccg.grids_num), 16, [&](const IndexRange range) {
    for (const int grid : range) {
      for (int y = 0; y < side-1; y++) {
        for (int x = 0; x < side-1; x++) {
          const int i = grid * area + y * side + x;
          const int indices[4] = {i, i+side, i+side+1, i+1};
          float3 face_normal(0.0f);
          for (int corner = 0; corner < 4; corner++) {
            add_newell_cross_v3_v3v3(face_normal,
                ccg.positions[indices[(corner+3)%4]], ccg.positions[indices[corner]]);
          }
          if (normalize_v3(face_normal) == 0.0f) {
            face_normal.z = 1.0f;
          }
          for (int corner = 0; corner < 4; corner++) {
            const int vert = indices[corner];
            const float3 prev = math::normalize(ccg.positions[indices[(corner+3)%4]] -
                                                ccg.positions[vert]);
            const float3 next = math::normalize(ccg.positions[indices[(corner+1)%4]] -
                                                ccg.positions[vert]);
            sums[vert] += face_normal * math::safe_acos_approx(math::dot(prev, next));
          }
        }
      }
    }
  });
  threading::parallel_for(IndexRange(ccg.grids_num), 16, [&](const IndexRange range) {
    SubdivCCGNeighbors neighbors;
    for (const int grid : range) {
      for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
          const int i = grid * area + y * side + x;
          float3 normal = sums[i];
          if (x == 0 || x == side-1 || y == 0 || y == side-1) {
            BKE_subdiv_ccg_neighbor_coords_get(ccg, {grid, short(x), short(y)}, true, neighbors);
            for (const SubdivCCGCoord &coord : neighbors.duplicates()) {
              normal += sums[coord.grid_index * area + coord.y * side + coord.x];
            }
          }
          ccg.normals[i] = math::normalize(normal);
        }
      }
    }
  });
}

bool shrinkwrap_ccg(Depsgraph *depsgraph,
                    Object &object,
                    const MultiresModifierData &mmd,
                    const Mesh &base_mesh,
                    SubdivCCG &ccg,
                    const bool respect_mask)
{
  if (!depsgraph || !shrinkwrap_enabled(mmd)) {
    return false;
  }
  ShrinkwrapModifierData smd = shrinkwrap_settings(mmd);
  const Array<float> weights = shrinkwrap_grid_weights(object, mmd, base_mesh, ccg, respect_mask);
  if (smd.shrinkType == MOD_SHRINKWRAP_PROJECT && smd.projAxis == 0) {
    shrinkwrap_grid_mesh_normals(ccg);
  }
  Array<float3> subdivided;
  if (smd.shrinkType == MOD_SHRINKWRAP_PROJECT && smd.projAxis == 0 && smd.subsurfLevels > 0) {
    subdivided = shrinkwrap_grid_projection_positions(ccg, base_mesh, smd.subsurfLevels);
  }
  const Span<float3> reference = subdivided.is_empty() ? ccg.positions.as_span() : subdivided.as_span();
  const ModifierEvalContext ctx{depsgraph, &object, ModifierApplyFlag(0)};
  return shrinkwrap_samples_deform(ctx, smd, ccg.positions, ccg.normals, reference, weights);
}

void polypaint_clear(MultiresModifierData &mmd)
{
  if (mmd.sculpt_color_grids) {
    for (int i = 0; i < mmd.sculpt_color_grids_num; i++) {
      MEM_SAFE_DELETE(mmd.sculpt_color_grids[i].colors);
    }
    MEM_delete(mmd.sculpt_color_grids);
  }
  mmd.sculpt_color_grids = nullptr;
  mmd.sculpt_color_grids_num = 0;
  mmd.sculpt_color_level = 0;
}

/* POLYPAINT_MULTI_ATTRIBUTE_V1: one active owner, inactive named owners.
 * Read views are borrowed and never alter the source or free its allocations. */
static bool polypaint_attribute_name_valid(const char *name)
{
  return name && name[0] && std::strlen(name) < sizeof(MultiresColorAttribute::name);
}

static MultiresColorAttribute *polypaint_attribute_find(MultiresModifierData &mmd,
                                                       const char *name)
{
  for (MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (STREQ(attribute.name, name)) {
      return &attribute;
    }
  }
  return nullptr;
}

bool polypaint_attribute_view(const MultiresModifierData &mmd,
                             const char *name, MultiresModifierData &r_view)
{
  if (!polypaint_attribute_name_valid(name)) {
    return false;
  }
  /* A legacy file has no name yet. Its only master belongs to the current
   * mesh color attribute; callers must only use this fallback for that name. */
  if (STREQ(mmd.sculpt_color_attribute, name) ||
      (!mmd.sculpt_color_attribute[0] && mmd.sculpt_color_attributes.is_empty())) {
    r_view.sculpt_color_grids = mmd.sculpt_color_grids;
    r_view.sculpt_color_grids_num = mmd.sculpt_color_grids_num;
    r_view.sculpt_color_level = mmd.sculpt_color_level;
    return true;
  }
  for (const MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (STREQ(attribute.name, name)) {
      r_view.sculpt_color_grids = attribute.grids;
      r_view.sculpt_color_grids_num = attribute.grids_num;
      r_view.sculpt_color_level = attribute.level;
      return true;
    }
  }
  return false;
}

bool polypaint_attribute_activate(MultiresModifierData &mmd, const char *name)
{
  if (!polypaint_attribute_name_valid(name)) {
    return false;
  }
  if (!mmd.sculpt_color_attribute[0]) {
    if (!mmd.sculpt_color_grids) {
      if (MultiresColorAttribute *target = polypaint_attribute_find(mmd, name)) {
        mmd.sculpt_color_grids = target->grids;
        mmd.sculpt_color_grids_num = target->grids_num;
        mmd.sculpt_color_level = target->level;
        BLI_remlink(&mmd.sculpt_color_attributes, target);
        MEM_delete(target);
      }
    }
    /* Migration only: the caller supplies the original mesh's current name
     * before selecting a different attribute. No old colors are reallocated. */
    BLI_strncpy(mmd.sculpt_color_attribute, name, sizeof(mmd.sculpt_color_attribute));
    return true;
  }
  if (STREQ(mmd.sculpt_color_attribute, name)) {
    return true;
  }
  MultiresColorAttribute *target = polypaint_attribute_find(mmd, name);
  /* Validate both directories before publishing the ownership exchange. */
  if ((mmd.sculpt_color_grids && (mmd.sculpt_color_level < 1 ||
                                 mmd.sculpt_color_level > 13 || mmd.sculpt_color_grids_num < 0)) ||
      (target && target->grids && (target->level < 1 || target->level > 13 ||
                                 target->grids_num < 0))) {
    return false;
  }
  auto *old = MEM_new<MultiresColorAttribute>("inactive Polypaint attribute");
  BLI_strncpy(old->name, mmd.sculpt_color_attribute, sizeof(old->name));
  old->grids = mmd.sculpt_color_grids;
  old->grids_num = mmd.sculpt_color_grids_num;
  old->level = mmd.sculpt_color_level;
  mmd.sculpt_color_grids = target ? target->grids : nullptr;
  mmd.sculpt_color_grids_num = target ? target->grids_num : 0;
  mmd.sculpt_color_level = target ? target->level : 0;
  BLI_strncpy(mmd.sculpt_color_attribute, name, sizeof(mmd.sculpt_color_attribute));
  if (target) {
    BLI_remlink(&mmd.sculpt_color_attributes, target);
    MEM_delete(target); /* Payload ownership has moved to the active slot. */
  }
  BLI_addtail(&mmd.sculpt_color_attributes, old);
  return true;
}

void polypaint_attribute_remove(MultiresModifierData &mmd, const char *name)
{
  if (!polypaint_attribute_name_valid(name)) {
    return;
  }
  if (STREQ(mmd.sculpt_color_attribute, name)) {
    polypaint_clear(mmd);
    mmd.sculpt_color_attribute[0] = '\0';
    return;
  }
  if (MultiresColorAttribute *attribute = polypaint_attribute_find(mmd, name)) {
    MultiresModifierData owned{};
    owned.sculpt_color_grids = attribute->grids;
    owned.sculpt_color_grids_num = attribute->grids_num;
    owned.sculpt_color_level = attribute->level;
    polypaint_clear(owned);
    BLI_remlink(&mmd.sculpt_color_attributes, attribute);
    MEM_delete(attribute);
  }
}

bool polypaint_attribute_rename(MultiresModifierData &mmd, const char *old_name,
                               const char *new_name)
{
  if (!polypaint_attribute_name_valid(old_name) || !polypaint_attribute_name_valid(new_name)) {
    return false;
  }
  if (STREQ(old_name, new_name)) {
    return true;
  }
  if (STREQ(mmd.sculpt_color_attribute, new_name) || polypaint_attribute_find(mmd, new_name)) {
    return false;
  }
  if (STREQ(mmd.sculpt_color_attribute, old_name)) {
    BLI_strncpy(mmd.sculpt_color_attribute, new_name, sizeof(mmd.sculpt_color_attribute));
  }
  else if (MultiresColorAttribute *attribute = polypaint_attribute_find(mmd, old_name)) {
    BLI_strncpy(attribute->name, new_name, sizeof(attribute->name));
  }
  return true;
}

void polypaint_attributes_clear(MultiresModifierData &mmd)
{
  while (!mmd.sculpt_color_attributes.is_empty()) {
    auto *attribute = static_cast<MultiresColorAttribute *>(mmd.sculpt_color_attributes.first);
    /* Do not use remove(name) here: malformed duplicate names must also free
     * the exact list entry, never the active allocation. */
    MultiresModifierData owned{};
    owned.sculpt_color_grids = attribute->grids;
    owned.sculpt_color_grids_num = attribute->grids_num;
    owned.sculpt_color_level = attribute->level;
    polypaint_clear(owned);
    BLI_remlink(&mmd.sculpt_color_attributes, attribute);
    MEM_delete(attribute);
  }
  polypaint_clear(mmd);
  mmd.sculpt_color_attribute[0] = '\0';
}

bool polypaint_ensure(MultiresModifierData &mmd, const int level, const int grids_num)
{
  if (level < 1 || level > 13 || grids_num < 0) {
    return false;
  }
  if (mmd.sculpt_color_grids) {
    /* A Sculpt Level change must NOT resample/destroy the high-res master.
     * Topology changes are handled by explicit remapping, never by clear(). */
    return mmd.sculpt_color_grids_num == grids_num &&
           mmd.sculpt_color_level >= 1 && mmd.sculpt_color_level <= 13;
  }
  if (grids_num > 0) {
    mmd.sculpt_color_grids = MEM_new_array_zeroed<MultiresColorGrid>(
        grids_num, "multires polypaint grids");
  }
  mmd.sculpt_color_grids_num = grids_num;
  mmd.sculpt_color_level = level;
  return true;
}

bool polypaint_lazy_master_enabled()
{
  const char *value = BLI_getenv("BLENDER_SCULPT_POLYPAINT_LAZY_MASTER");
  return value && value[0] == '1' && value[1] == '\0';
}

bool polypaint_stage_master(const MultiresModifierData &source,
                           const int level,
                           const int corners_num,
                           MultiresModifierData &destination)
{
  if (level < 1 || level > 13 || corners_num <= 0 ||
      destination.sculpt_color_grids || destination.sculpt_color_grids_num != 0 ||
      destination.sculpt_color_level != 0)
  {
    return false;
  }
  const int old_level = source.sculpt_color_level;
  if (source.sculpt_color_grids) {
    if (old_level < 1 || old_level > 13 || level <= old_level ||
        source.sculpt_color_grids_num != corners_num)
    {
      return false;
    }
    const int old_side = subdiv::grid_size_from_level(old_level);
    for (int corner = 0; corner < corners_num; corner++) {
      const MultiresColorGrid &grid = source.sculpt_color_grids[corner];
      if (!grid.colors) {
        continue;
      }
      if (grid.level != old_level || grid.totcolor != old_side * old_side) {
        return false;
      }
      for (int sample = 0; sample < grid.totcolor; sample++) {
        for (int channel = 0; channel < 4; channel++) {
          if (!std::isfinite(grid.colors[sample][channel])) {
            return false;
          }
        }
      }
    }
  }
  else if (old_level != 0 || source.sculpt_color_grids_num != 0) {
    return false;
  }

  /* Own all staged buffers until the editor's Undo boundary succeeds. Neither
   * the live master nor earlier Color Undo snapshots are resized in place. */
  if (!polypaint_ensure(destination, level, corners_num)) {
    return false;
  }
  const int new_side = subdiv::grid_size_from_level(level);
  const int old_side = source.sculpt_color_grids ? subdiv::grid_size_from_level(old_level) : 0;
  const int step = old_side ? (new_side - 1) / (old_side - 1) : 0;
  for (int corner = 0; corner < corners_num; corner++) {
    MultiresColorGrid &grid = destination.sculpt_color_grids[corner];
    grid.level = level;
    grid.totcolor = new_side * new_side;
    if (!source.sculpt_color_grids || !source.sculpt_color_grids[corner].colors) {
      continue;
    }
    const float (*old)[4] = source.sculpt_color_grids[corner].colors;
    grid.colors = MEM_new_array_uninitialized<float[4]>(grid.totcolor,
                                                       "staged polypaint master");
    for (int y = 0; y < new_side; y++) {
      const int y0 = y / step, y1 = std::min(y0 + 1, old_side - 1);
      const double fy = double(y % step) / step;
      for (int x = 0; x < new_side; x++) {
        const int x0 = x / step, x1 = std::min(x0 + 1, old_side - 1);
        if (x % step == 0 && y % step == 0) {
          memcpy(grid.colors[y * new_side + x], old[y0 * old_side + x0], sizeof(float[4]));
          continue;
        }
        const double fx = double(x % step) / step;
        for (int channel = 0; channel < 4; channel++) {
          const double a = double(old[y0 * old_side + x0][channel]) * (1.0 - fx) +
                           double(old[y0 * old_side + x1][channel]) * fx;
          const double b = double(old[y1 * old_side + x0][channel]) * (1.0 - fx) +
                           double(old[y1 * old_side + x1][channel]) * fx;
          grid.colors[y * new_side + x][channel] = float(a * (1.0 - fy) + b * fy);
        }
      }
    }
  }
  return true;
}

void polypaint_swap_master(MultiresModifierData &a, MultiresModifierData &b)
{
  std::swap(a.sculpt_color_grids, b.sculpt_color_grids);
  std::swap(a.sculpt_color_grids_num, b.sculpt_color_grids_num);
  std::swap(a.sculpt_color_level, b.sculpt_color_level);
}

float (*polypaint_grid_ensure_master(MultiresModifierData &mmd, const int corner))[4]
{
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num ||
      mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13)
  {
    return nullptr;
  }
  MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  const int area = side * side;
  if (grid.colors && (grid.level != mmd.sculpt_color_level || grid.totcolor != area)) {
    /* Fail closed: never return malformed storage to brush/undo writers. */
    return nullptr;
  }
  if (!grid.colors) {
    grid.colors = MEM_new_array_uninitialized<float[4]>(area, "multires polypaint color");
    for (int i = 0; i < area; i++) {
      grid.colors[i][0] = grid.colors[i][1] = grid.colors[i][2] = grid.colors[i][3] = 1.0f;
    }
    grid.totcolor = area;
    grid.level = mmd.sculpt_color_level;
  }
  return grid.colors;
}

float (*polypaint_grid_ensure(MultiresModifierData &mmd, const int corner))[4]
{
  /* Legacy writers index using the current CCG side. Never allow those writes
   * at a different (lower or higher) level than the stored color master. */
  return mmd.sculptlvl == mmd.sculpt_color_level ? polypaint_grid_ensure_master(mmd, corner) :
                                                  nullptr;
}

const float (*polypaint_grid(const MultiresModifierData &mmd, const int corner))[4]
{
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num ||
      mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13)
  {
    return nullptr;
  }
  const MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  return grid.colors && grid.level == mmd.sculpt_color_level && grid.totcolor == side * side ?
             grid.colors :
             nullptr;
}

bool polypaint_sample_grid(const MultiresColorGrid &grid,
                           const float u,
                           const float v,
                           float r_rgba[4])
{
  if (r_rgba == nullptr) {
    return false;
  }
  r_rgba[0] = r_rgba[1] = r_rgba[2] = r_rgba[3] = 1.0f;
  if (!grid.colors || grid.level < 1 || grid.level > 13 || !std::isfinite(u) ||
      !std::isfinite(v))
  {
    return false;
  }
  const int side = subdiv::grid_size_from_level(grid.level);
  if (side < 2 || grid.totcolor != side * side) {
    return false;
  }
  const float x = std::clamp(u, 0.0f, 1.0f) * float(side - 1);
  const float y = std::clamp(v, 0.0f, 1.0f) * float(side - 1);
  const int x0 = int(x), y0 = int(y);
  const int x1 = std::min(x0 + 1, side - 1), y1 = std::min(y0 + 1, side - 1);
  const float tx = x - float(x0), ty = y - float(y0);
  for (int channel = 0; channel < 4; channel++) {
    const float a = grid.colors[y0 * side + x0][channel] * (1.0f - tx) +
                    grid.colors[y0 * side + x1][channel] * tx;
    const float b = grid.colors[y1 * side + x0][channel] * (1.0f - tx) +
                    grid.colors[y1 * side + x1][channel] * tx;
    r_rgba[channel] = a * (1.0f - ty) + b * ty;
  }
  return true;
}

bool polypaint_sample(const MultiresModifierData &mmd,
                      const int corner,
                      const float u,
                      const float v,
                      float r_rgba[4])
{
  if (r_rgba == nullptr) {
    return false;
  }
  r_rgba[0] = r_rgba[1] = r_rgba[2] = r_rgba[3] = 1.0f;
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num) {
    return false;
  }
  return polypaint_sample_grid(mmd.sculpt_color_grids[corner], u, v, r_rgba);
}

bool polypaint_grid_discard(MultiresModifierData &mmd, const int corner)
{
  if (!mmd.sculpt_color_grids || corner < 0 || corner >= mmd.sculpt_color_grids_num) {
    return false;
  }
  MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
  MEM_SAFE_DELETE(grid.colors);
  grid.totcolor = 0;
  grid.level = mmd.sculpt_color_level;
  return true;
}

bool polypaint_resample(MultiresModifierData &mmd, const int new_level)
{
  if (!mmd.sculpt_color_grids || new_level < 1 || new_level > 13 ||
      mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13)
  {
    return false;
  }
  if (new_level == mmd.sculpt_color_level) {
    return true;
  }
  const int old_side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  const int new_side = subdiv::grid_size_from_level(new_level);
  /* Preflight all grids before changing any: invalid metadata would otherwise
   * read past the source and partially corrupt the persistent color master. */
  for (int corner = 0; corner < mmd.sculpt_color_grids_num; corner++) {
    const MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
    if (grid.colors && (grid.level != mmd.sculpt_color_level ||
                        grid.totcolor != old_side * old_side))
    {
      return false;
    }
  }
  for (int corner = 0; corner < mmd.sculpt_color_grids_num; corner++) {
    MultiresColorGrid &grid = mmd.sculpt_color_grids[corner];
    if (!grid.colors) {
      continue;
    }
    float (*colors)[4] = MEM_new_array_uninitialized<float[4]>(new_side * new_side,
                                                               "resampled polypaint");
    for (int y = 0; y < new_side; y++) {
      const float v = float(y) * float(old_side - 1) / float(new_side - 1);
      const int y0 = int(v), y1 = std::min(y0 + 1, old_side - 1);
      const float fy = v - y0;
      for (int x = 0; x < new_side; x++) {
        const float u = float(x) * float(old_side - 1) / float(new_side - 1);
        const int x0 = int(u), x1 = std::min(x0 + 1, old_side - 1);
        const float fx = u - x0;
        for (int c = 0; c < 4; c++) {
          const float a = grid.colors[y0 * old_side + x0][c] * (1.0f - fx) +
                          grid.colors[y0 * old_side + x1][c] * fx;
          const float b = grid.colors[y1 * old_side + x0][c] * (1.0f - fx) +
                          grid.colors[y1 * old_side + x1][c] * fx;
          colors[y * new_side + x][c] = a * (1.0f - fy) + b * fy;
        }
      }
    }
    MEM_delete(grid.colors);
    grid.colors = colors;
    grid.totcolor = new_side * new_side;
    grid.level = new_level;
  }
  mmd.sculpt_color_level = new_level;
  return true;
}



/* POLYPAINT_V02A_CPU_FIRST: never synthesize white grids on a missing source.
 * This reads the original Mesh attribute at per-face-corner resolution.
 * Shared CCG edge values follow the same face-center/edge-midpoint convention
 * as the CPU-side subdivision attribute approximation. */
static bool polypaint_base_patch(const Mesh &base_mesh,
                                 const char *attribute_name,
                                 const int corner,
                                 float r_patch[4][4])
{
  if (!attribute_name || attribute_name[0] == '\0' || corner < 0 ||
      corner >= base_mesh.corners_num)
  {
    return false;
  }
  const VArray<ColorGeometry4f> colors = *base_mesh.attributes().lookup<ColorGeometry4f>(
      attribute_name, AttrDomain::Corner);
  if (!colors || colors.size() != base_mesh.corners_num) {
    return false;
  }
  const Span<int> corner_faces = base_mesh.corner_to_face_map();
  if (corner >= corner_faces.size()) {
    return false;
  }
  const int face_index = corner_faces[corner];
  const OffsetIndices<int> faces = base_mesh.faces();
  if (face_index < 0 || face_index >= faces.size()) {
    return false;
  }
  const IndexRange face = faces[face_index];
  const int count = face.size();
  if (count < 3 || corner < face.start() || corner >= face.one_after_last()) {
    return false;
  }
  const int offset = corner - face.start();
  const int next = face.start() + (offset + 1) % count;
  const int prev = face.start() + (offset + count - 1) % count;
  const ColorGeometry4f a = colors[corner];
  const ColorGeometry4f b = colors[next];
  const ColorGeometry4f d = colors[prev];
  const float values_a[4] = {a.r, a.g, a.b, a.a};
  const float values_b[4] = {b.r, b.g, b.b, b.a};
  const float values_d[4] = {d.r, d.g, d.b, d.a};
  for (int c = 0; c < 4; c++) {
    r_patch[0][c] = 0.0f;
    r_patch[1][c] = (values_a[c] + values_b[c]) * 0.5f;
    r_patch[2][c] = (values_a[c] + values_d[c]) * 0.5f;
    r_patch[3][c] = values_a[c];
  }
  for (const int face_corner : face) {
    const ColorGeometry4f value = colors[face_corner];
    r_patch[0][0] += value.r / float(count);
    r_patch[0][1] += value.g / float(count);
    r_patch[0][2] += value.b / float(count);
    r_patch[0][3] += value.a / float(count);
  }
  return true;
}

bool polypaint_sample_with_base(const MultiresModifierData &mmd,
                                const Mesh &base_mesh,
                                const char *attribute_name,
                                const int corner,
                                const float u,
                                const float v,
                                float r_rgba[4])
{
  if (!r_rgba || !std::isfinite(u) || !std::isfinite(v) ||
      corner < 0 || corner >= base_mesh.corners_num)
  {
    return false;
  }
  /* Existing true color values, including painted white, take precedence. */
  const bool active = !mmd.sculpt_color_attribute[0] ||
      (attribute_name && STREQ(mmd.sculpt_color_attribute, attribute_name));
  if (active) {
    /* The hot brush path reads the active slot directly: no directory search
     * or modifier-sized temporary for every painted sample. */
    if (polypaint_sample(mmd, corner, u, v, r_rgba)) {
      return true;
    }
  }
  else {
    MultiresModifierData view{};
    if (polypaint_attribute_view(mmd, attribute_name, view) &&
        polypaint_sample(view, corner, u, v, r_rgba)) {
      return true;
    }
  }
  float patch[4][4];
  if (!polypaint_base_patch(base_mesh, attribute_name, corner, patch)) {
    return false;
  }
  multires_polypaint_cpu::sample_patch(patch, u, v, r_rgba);
  return true;
}

float (*polypaint_grid_ensure_from_base(MultiresModifierData &mmd,
                                        const Mesh &base_mesh,
                                        const char *attribute_name,
                                        const int corner))[4]
{
  if (mmd.sculpt_color_attribute[0] &&
      (!attribute_name || !STREQ(mmd.sculpt_color_attribute, attribute_name))) {
    return nullptr;
  }
  if (corner < 0 || corner >= base_mesh.corners_num || mmd.totlvl < 1 ||
      mmd.totlvl > 13)
  {
    return nullptr;
  }
  /* Preflight topology before any resample: no partial mutation on mismatched meshes. */
  if (mmd.sculpt_color_grids && mmd.sculpt_color_grids_num != base_mesh.corners_num) {
    return nullptr;
  }
  if (polypaint_lazy_master_enabled() && !mmd.sculpt_color_grids) {
    /* A writer cannot establish an Undo boundary. Stroke entry owns creation. */
    return nullptr;
  }
  /* A color master older than the geometric maximum must be promoted before any
   * edit. Returning an existing lower-resolution grid here would silently cap
   * subsequent painting at its former level. */
  if (mmd.sculpt_color_grids && mmd.sculpt_color_level < mmd.totlvl) {
    if (polypaint_lazy_master_enabled()) {
      /* The main-thread stroke entry must prepare before parallel Undo capture.
       * Geometry may be finer; only the actual Paint level needs coverage. */
      if (mmd.sculpt_color_level < mmd.sculptlvl) {
        return nullptr;
      }
    }
    else if (!polypaint_resample(mmd, mmd.totlvl)) {
      return nullptr;
    }
    if (polypaint_grid(mmd, corner)) {
      return mmd.sculpt_color_grids[corner].colors;
    }
  }
  /* Existing painted data is authoritative, including explicit white. */
  if (mmd.sculpt_color_grids && polypaint_grid(mmd, corner)) {
    return mmd.sculpt_color_grids[corner].colors;
  }
  /* Delete Higher may leave an independent color master above the geometric
   * maximum. Materialize a previously untouched corner at that stored stride,
   * then let the coarse writer apply its interpolated difference as usual. */
  const int master_level = mmd.sculpt_color_grids ? mmd.sculpt_color_level : mmd.totlvl;
  if (master_level < 1 || master_level > 13) {
    return nullptr;
  }
  float patch[4][4];
  if (!polypaint_base_patch(base_mesh, attribute_name, corner, patch)) {
    return nullptr;
  }
  if (!polypaint_ensure(mmd, master_level, base_mesh.corners_num) ||
      mmd.sculpt_color_level != master_level)
  {
    return nullptr;
  }
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  float (*master)[4] = polypaint_grid_ensure_master(mmd, corner);
  if (!master) {
    return nullptr;
  }
  /* polypaint_grid_ensure_master() is a no-op for a populated grid. Only the
   * newly allocated case reaches here, because it was tested above. */
  for (int y = 0; y < side; y++) {
    for (int x = 0; x < side; x++) {
      multires_polypaint_cpu::sample_patch(
          patch, float(x) / float(side - 1), float(y) / float(side - 1),
          master[y * side + x]);
    }
  }
  return master;
}

bool polypaint_write_coarse_sample(MultiresModifierData &mmd,
                                   const Mesh &base_mesh,
                                   const char *attribute_name,
                                   const int corner,
                                   const int paint_level,
                                   const int x,
                                   const int y,
                                   const float rgba[4])
{
  if (!rgba || paint_level < 1 || paint_level > mmd.totlvl ||
      paint_level > 13 || x < 0 || y < 0)
  {
    return false;
  }
  const int paint_side = subdiv::grid_size_from_level(paint_level);
  if (x >= paint_side || y >= paint_side) {
    return false;
  }
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(rgba[c])) {
      return false;
    }
  }
  float (*master)[4] = polypaint_grid_ensure_from_base(mmd, base_mesh, attribute_name, corner);
  if (!master) {
    return false;
  }
  const int side = subdiv::grid_size_from_level(mmd.sculpt_color_level);
  const float u = float(x) / float(paint_side - 1);
  const float v = float(y) / float(paint_side - 1);
  float current[4];
  if (!multires_polypaint_cpu::sample_grid(master, side, u, v, current)) {
    return false;
  }
  float delta[4];
  for (int c = 0; c < 4; c++) {
    delta[c] = rgba[c] - current[c];
  }
  return multires_polypaint_cpu::add_coarse_delta(
      master, side, paint_side, x, y, delta);
}

void faceset_clear(MultiresModifierData &mmd)
{
  if (mmd.sculpt_face_set_grids) {
    for (int i = 0; i < mmd.sculpt_face_set_grids_num; i++) {
      MEM_SAFE_DELETE(mmd.sculpt_face_set_grids[i].ids);
    }
    MEM_delete(mmd.sculpt_face_set_grids);
  }
  mmd.sculpt_face_set_grids = nullptr;
  mmd.sculpt_face_set_grids_num = 0;
  mmd.sculpt_face_set_level = 0;
}

bool faceset_ensure_from_mesh(MultiresModifierData &mmd, const Mesh &mesh, const int level)
{
  if (level < 1 || level > 13 || mesh.corners_num < 0) {
    return false;
  }

  if (mmd.sculpt_face_set_grids) {
    if (mmd.sculpt_face_set_grids_num != mesh.corners_num) {
      return false;
    }
    if (level > mmd.sculpt_face_set_level) {
      return faceset_resample_up(mmd, level);
    }
    /* The existing higher-resolution master remains authoritative. */
    return true;
  }

  const AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> face_sets = *attributes.lookup_or_default<int>(
      ".sculpt_face_set", AttrDomain::Face, 1);

  mmd.sculpt_face_set_grids = mesh.corners_num == 0 ? nullptr :
      MEM_new_array<MultiresFaceSetGrid>(mesh.corners_num, "multires face set grids");
  mmd.sculpt_face_set_grids_num = mesh.corners_num;
  mmd.sculpt_face_set_level = level;

  const OffsetIndices<int> faces = mesh.faces();
  for (const int face : faces.index_range()) {
    const int id = face_sets[face];
    for (const int corner : faces[face]) {
      MultiresFaceSetGrid &grid = mmd.sculpt_face_set_grids[corner];
      grid.level = level;
      grid.uniform_id = id;
    }
  }
  return true;
}

int *faceset_grid_ensure_dense(MultiresModifierData &mmd, const int corner)
{
  if (!mmd.sculpt_face_set_grids || corner < 0 || corner >= mmd.sculpt_face_set_grids_num ||
      mmd.sculpt_face_set_level < 1 || mmd.sculpt_face_set_level > 13)
  {
    return nullptr;
  }
  MultiresFaceSetGrid &grid = mmd.sculpt_face_set_grids[corner];
  const int side = subdiv::grid_size_from_level(mmd.sculpt_face_set_level);
  const int area = side * side;
  if (!grid.ids) {
    grid.ids = MEM_new_array_uninitialized<int>(area, "multires face set ids");
    std::fill_n(grid.ids, area, grid.uniform_id);
    grid.totid = area;
    grid.level = mmd.sculpt_face_set_level;
  }
  return grid.ids;
}

const int *faceset_grid(const MultiresModifierData &mmd, const int corner)
{
  if (!mmd.sculpt_face_set_grids || corner < 0 || corner >= mmd.sculpt_face_set_grids_num) {
    return nullptr;
  }
  return mmd.sculpt_face_set_grids[corner].ids;
}

bool faceset_resample_up(MultiresModifierData &mmd, const int new_level)
{
  if (!mmd.sculpt_face_set_grids || mmd.sculpt_face_set_level < 1 ||
      mmd.sculpt_face_set_level > 13 || new_level < 1 || new_level > 13)
  {
    return false;
  }
  if (new_level <= mmd.sculpt_face_set_level) {
    /* Never destructively downsample the master. */
    return true;
  }

  const int old_side = subdiv::grid_size_from_level(mmd.sculpt_face_set_level);
  const int new_side = subdiv::grid_size_from_level(new_level);
  for (int corner = 0; corner < mmd.sculpt_face_set_grids_num; corner++) {
    MultiresFaceSetGrid &grid = mmd.sculpt_face_set_grids[corner];
    if (!grid.ids) {
      grid.level = new_level;
      continue;
    }
    if (grid.totid != old_side * old_side || grid.level != mmd.sculpt_face_set_level) {
      return false;
    }

    int *ids = MEM_new_array_uninitialized<int>(new_side * new_side,
                                                "resampled multires face sets");
    for (int y = 0; y < new_side; y++) {
      const int sy = int(std::round(float(y) * float(old_side - 1) / float(new_side - 1)));
      for (int x = 0; x < new_side; x++) {
        const int sx = int(std::round(float(x) * float(old_side - 1) / float(new_side - 1)));
        ids[y * new_side + x] = grid.ids[sy * old_side + sx];
      }
    }
    MEM_delete(grid.ids);
    grid.ids = ids;
    grid.totid = new_side * new_side;
    grid.level = new_level;
  }
  mmd.sculpt_face_set_level = new_level;
  return true;
}

int faceset_sample(const MultiresModifierData &mmd,
                   const int corner,
                   const float grid_u,
                   const float grid_v,
                   const int fallback_id)
{
  if (!mmd.sculpt_face_set_grids || corner < 0 || corner >= mmd.sculpt_face_set_grids_num ||
      mmd.sculpt_face_set_level < 1 || mmd.sculpt_face_set_level > 13)
  {
    return fallback_id;
  }
  const MultiresFaceSetGrid &grid = mmd.sculpt_face_set_grids[corner];
  if (!grid.ids) {
    return grid.uniform_id;
  }
  const int side = subdiv::grid_size_from_level(mmd.sculpt_face_set_level);
  if (grid.totid != side * side || grid.level != mmd.sculpt_face_set_level) {
    return fallback_id;
  }
  const int x = std::clamp(int(std::round(grid_u * float(side - 1))), 0, side - 1);
  const int y = std::clamp(int(std::round(grid_v * float(side - 1))), 0, side - 1);
  return grid.ids[y * side + x];
}

bool commit(Object &object, bool *r_local_surface_preserved)
{
  if (r_local_surface_preserved) {
    *r_local_surface_preserved = false;
  }
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss && (ss->cache || ss->filter_cache)) {
    return false;
  }
  return multires_flush_sculpt_updates(&object, r_local_surface_preserved);
}

bool compatible(const MultiresModifierData &mmd, const Mesh &mesh)
{
  if (mmd.totlvl > 13) {
    return false;
  }
  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh.corner_data, CD_MDISPS));
  if (mmd.totlvl > 0 && base == nullptr) {
    return false;
  }
  const int side = mmd.totlvl > 0 ? subdiv::grid_size_from_level(mmd.totlvl) : 0;
  for (int corner = 0; mmd.totlvl > 0 && corner < mesh.corners_num; corner++) {
    if (!base[corner].disps || base[corner].level != mmd.totlvl ||
        base[corner].totdisp != side * side) {
      return false;
    }
  }
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if ((layer.base_deltas && layer.base_verts_num != mesh.verts_num) ||
        !std::isfinite(layer.strength)) {
      return false;
    }
    if (mmd.totlvl == 0) {
      if (layer.disps || layer.grids_num != 0 || layer.level != 0) {
        return false;
      }
      continue;
    }
    if (!layer.disps || layer.level != mmd.totlvl ||
        layer.grids_num != mesh.corners_num || !std::isfinite(layer.strength)) {
      return false;
    }
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      if (layer.disps[corner].level != mmd.totlvl ||
          layer.disps[corner].totdisp != side * side) {
        return false;
      }
    }
  }
  return true;
}

bool resample_all(MultiresModifierData &mmd, const int new_level, const int grids_num)
{
  weight_hot_delta_cache_invalidate();
  if (new_level <= 0 || new_level > 13 || grids_num <= 0) {
    return false;
  }
  const int new_side = subdiv::grid_size_from_level(new_level);
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.level <= 0 || layer.level >= new_level || layer.grids_num != grids_num ||
        !layer.disps)
    {
      return false;
    }
    const int old_side = subdiv::grid_size_from_level(layer.level);
    for (int corner = 0; corner < grids_num; corner++) {
      const MDisps &grid = layer.disps[corner];
      if (!grid.disps || grid.level != layer.level || grid.totdisp != old_side * old_side) {
        return false;
      }
      for (int i = 0; i < grid.totdisp; i++) {
        for (int axis = 0; axis < 3; axis++) {
          if (!std::isfinite(grid.disps[i][axis])) {
            return false;
          }
        }
      }
    }
  }

  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    const int old_side = subdiv::grid_size_from_level(layer.level);
    /* Input validation above covers every layer before any mutation. Process
     * one grid at a time so old and new full-layer buffers never coexist.
     * There are no recoverable failure exits after this point. */
    for (int corner = 0; corner < grids_num; corner++) {
      MDisps &old_grid = layer.disps[corner];
      MDisps new_grid{};
      new_grid.level = new_level;
      new_grid.totdisp = new_side * new_side;
      /* Every component is assigned below before the buffer is published. */
      new_grid.disps = MEM_new_array_uninitialized<float[3]>(
          new_grid.totdisp, "resampled sculpt layer displacement");
      if (old_grid.hidden) {
        new_grid.hidden = BLI_BITMAP_NEW(new_grid.totdisp, "resampled sculpt layer hidden");
      }
      for (int y = 0; y < new_side; y++) {
        const float v = float(y) * float(old_side - 1) / float(new_side - 1);
        const int y0 = int(v), y1 = std::min(y0 + 1, old_side - 1);
        const float fy = v - float(y0);
        for (int x = 0; x < new_side; x++) {
          const float u = float(x) * float(old_side - 1) / float(new_side - 1);
          const int x0 = int(u), x1 = std::min(x0 + 1, old_side - 1);
          const float fx = u - float(x0);
          const int dst = y * new_side + x;
          const int i00 = y0 * old_side + x0, i10 = y0 * old_side + x1;
          const int i01 = y1 * old_side + x0, i11 = y1 * old_side + x1;
          for (int axis = 0; axis < 3; axis++) {
            const float a = old_grid.disps[i00][axis] * (1.0f - fx) +
                            old_grid.disps[i10][axis] * fx;
            const float b = old_grid.disps[i01][axis] * (1.0f - fx) +
                            old_grid.disps[i11][axis] * fx;
            new_grid.disps[dst][axis] = a * (1.0f - fy) + b * fy;
          }
          if (old_grid.hidden) {
            const int sx = int(std::round(u)), sy = int(std::round(v));
            BLI_BITMAP_SET(new_grid.hidden, dst,
                           BLI_BITMAP_TEST_BOOL(old_grid.hidden, sy * old_side + sx));
          }
        }
      }
      MEM_SAFE_DELETE(old_grid.disps);
      MEM_SAFE_DELETE(old_grid.hidden);
      old_grid = new_grid;
    }
    layer.level = new_level;
    layer.grids_num = grids_num;
  }
  return polypaint_lazy_master_enabled() || !mmd.sculpt_color_grids ||
         polypaint_resample(mmd, new_level);
}

bool can_record(const MultiresModifierData &mmd, const Mesh &mesh)
{
  const MultiresSculptLayer *layer = active(mmd);
  return layer && (layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) &&
         !(mmd.flags & eMultiresModifierFlag_UseSculptBaseMesh) &&
         std::isfinite(layer->strength) && std::abs(layer->strength) >= 1e-6f &&
         mmd.sculptlvl <= mmd.totlvl && compatible(mmd, mesh);
}

const char *merge_visible_to_base(MultiresModifierData &mmd, Mesh &mesh,
                                  const bool remove_layers, const bool validate_only)
{
  /* MERGE_VISIBLE_NATIVE_BASE_V1. Keep the original cage/tangent frames.
   * All validations precede CustomData writable access and layer removal.
   * Once published, no further operation in this transaction can fail. */
  if (mesh.id.lib || ID_REAL_USERS(&mesh.id) > 1 || mesh.key ||
      CustomData_external_test(&mesh.corner_data, CD_MDISPS))
  {
    return "Merge requires a single-user mesh without shape keys or external displacement";
  }
  if (mmd.totlvl <= 0 || !compatible(mmd, mesh)) {
    return "Merge requires valid native and layer grids at the maximum Multires level";
  }
  Vector<MultiresSculptLayer *> visible;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE)) {
      continue;
    }
    visible.append(&layer);
    if (!is_effective(layer) || !layer.base_deltas) {
      continue;
    }
    for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
      for (int axis = 0; axis < 3; axis++) {
        if (layer.base_deltas[vertex][axis] != 0.0f) {
          return "Visible L0 cage edits require frame rebasing; original layers retained";
        }
      }
    }
  }
  if (visible.is_empty()) {
    return "No visible Sculpt Layers to merge";
  }
  const MDisps *base = static_cast<const MDisps *>(
      CustomData_get_layer(&mesh.corner_data, CD_MDISPS));
  /* Match native compositor order, zero-strength threshold and sparse reads.
   * Deterministic two-pass computation needs no additional high-resolution
   * staging buffer. A shared CustomData buffer is detached only on publication. */
  for (int corner = 0; corner < mesh.corners_num; corner++) {
    for (int sample = 0; sample < base[corner].totdisp; sample++) {
      for (int axis = 0; axis < 3; axis++) {
        float value = base[corner].disps[sample][axis];
        if (!std::isfinite(value)) {
          return "Non-finite native displacement; original layers retained";
        }
        for (const MultiresSculptLayer *layer : visible) {
          const MDisps &grid = layer->disps[corner];
          if (is_effective(*layer) && grid.disps) {
            value += grid.disps[sample][axis] * layer->strength;
            if (!std::isfinite(value)) {
              return "Non-finite merged displacement; original layers retained";
            }
          }
        }
      }
    }
  }

  if (validate_only) {
    return nullptr;
  }

  MDisps *destination = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh.corner_data, CD_MDISPS, mesh.corners_num));
  if (!destination) {
    return "Native displacement is not writable; original layers retained";
  }
  weight_hot_delta_cache_invalidate();
  for (int corner = 0; corner < mesh.corners_num; corner++) {
    for (int sample = 0; sample < destination[corner].totdisp; sample++) {
      for (int axis = 0; axis < 3; axis++) {
        float value = destination[corner].disps[sample][axis];
        for (const MultiresSculptLayer *layer : visible) {
          const MDisps &grid = layer->disps[corner];
          if (is_effective(*layer) && grid.disps) {
            value += grid.disps[sample][axis] * layer->strength;
          }
        }
        destination[corner].disps[sample][axis] = value;
      }
    }
  }
  const MultiresSculptLayer *selected = active(mmd);
  const bool keep_selected = selected && !(selected->flag & MULTIRES_SCULPT_LAYER_VISIBLE);
  for (MultiresSculptLayer *layer : visible) {
    if (remove_layers) {
      remove(mmd, *layer);
    }
    else {
      for (int corner = 0; corner < layer->grids_num; corner++) {
        MEM_SAFE_DELETE(layer->disps[corner].disps);
      }
      MEM_SAFE_DELETE(layer->base_deltas);
      layer->base_verts_num = 0;
    }
  }
  if (remove_layers && keep_selected) {
    mmd.sculpt_layer_active = BLI_findindex(&mmd.sculpt_layers, selected);
  }
  mmd.sculpt_layer_recording = 0;
  return nullptr;
}

bool bake_visible_to_base(MultiresModifierData &mmd, Mesh &mesh)
{
  weight_hot_delta_cache_invalidate();
  if (!compatible(mmd, mesh) || ID_REAL_USERS(&mesh.id) > 1) {
    return false;
  }
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE)) {
      return false;
    }
  }
  Array<float3> baked_positions(mesh.vert_positions());
  if (!base_deltas_apply(mmd, baked_positions)) {
    return false;
  }
  MDisps *base = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh.corner_data, CD_MDISPS, mesh.corners_num));
  /* First validate every resulting component, then apply the same calculation.
   * Layer entries are cleared only after their contribution has been read. */
  for (int pass = 0; pass < 2; pass++) {
    for (int corner = 0; mmd.totlvl > 0 && corner < mesh.corners_num; corner++) {
      for (int i = 0; i < base[corner].totdisp; i++) {
        for (int axis = 0; axis < 3; axis++) {
          float value = base[corner].disps[i][axis];
          if (!std::isfinite(value)) {
            return false;
          }
          for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
            value += layer.disps[corner].disps[i][axis] * layer.strength;
            if (!std::isfinite(value)) {
              return false;
            }
          }
          if (pass == 1) {
            base[corner].disps[i][axis] = value;
            for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
              layer.disps[corner].disps[i][axis] = 0.0f;
            }
          }
        }
      }
    }
  }
  mesh.vert_positions_for_write().copy_from(baked_positions);
  mesh.tag_positions_changed();
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.base_deltas) {
      memset(layer.base_deltas, 0, sizeof(float[3]) * layer.base_verts_num);
    }
  }
  mmd.sculpt_layer_recording = 0;
  return true;
}

static void free_layer(MultiresSculptLayer &layer)
{
  weight_hot_delta_cache_invalidate(&layer);
  MEM_SAFE_DELETE(layer.base_deltas);
  layer.base_verts_num = 0;
  if (layer.disps != nullptr) {
    for (int i = 0; i < layer.grids_num; i++) {
      MEM_SAFE_DELETE(layer.disps[i].disps);
      MEM_SAFE_DELETE(layer.disps[i].hidden);
    }
    MEM_delete(layer.disps);
    layer.disps = nullptr;
  }
  layer.grids_num = 0;
}

static void allocate_layer_grids(MultiresSculptLayer &layer, const int level, const int grids_num)
{
  float (*base_deltas)[3] = layer.base_deltas;
  const int base_verts_num = layer.base_verts_num;
  layer.base_deltas = nullptr;
  free_layer(layer);
  layer.base_deltas = base_deltas;
  layer.base_verts_num = base_verts_num;
  layer.level = level;
  layer.grids_num = level > 0 ? grids_num : 0;
  if (grids_num <= 0 || level <= 0) {
    return;
  }
  const int grid_size = subdiv::grid_size_from_level(level);
  const int grid_area = grid_size * grid_size;
  layer.disps = MEM_new_array_zeroed<MDisps>(grids_num, "multires sculpt layer grids");

  /* V4.5b CORE lazy payloads.
   * A null per-grid payload is an implicit all-zero Sculpt Layer grid.
   * The high-resolution float[3] buffer is allocated by grid_ensure() only
   * when a dirty grid actually receives layer data. */
  for (int i = 0; i < grids_num; i++) {
    layer.disps[i].totdisp = grid_area;
    layer.disps[i].level = level;
    layer.disps[i].disps = nullptr;
  }
}

void initialize_layer_grids(MultiresModifierData &mmd, const int level, const int corners_num)
{
  weight_hot_delta_cache_invalidate();
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    allocate_layer_grids(layer, level, corners_num);
  }
}

bool remap_surviving_corners(MultiresModifierData &mmd,
                            const int old_corners_num,
                            const Span<int> new_to_old)
{
  weight_hot_delta_cache_invalidate();
  if (old_corners_num < 0 || new_to_old.size() > INT_MAX ||
      mmd.totlvl < 1 || mmd.totlvl > 13)
  {
    return false;
  }
  /* A copied/interpolated edit-mesh index is not proof of a surviving corner.
   * Require a one-to-one map so duplicated faces cannot silently inherit detail. */
  Array<bool> used(old_corners_num, false);
  for (const int old_corner : new_to_old) {
    if (old_corner < -1 || old_corner >= old_corners_num) {
      return false;
    }
    if (old_corner >= 0) {
      if (used[old_corner]) {
        return false;
      }
      used[old_corner] = true;
    }
  }
  const int side = subdiv::grid_size_from_level(mmd.totlvl);
  const int area = side * side;
  /* Validate all companion storage before publishing any layer changes. */
  for (const MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (attribute.grids && (attribute.grids_num != old_corners_num ||
                            attribute.level < 1 || attribute.level > 13)) {
      return false;
    }
  }
  if (mmd.sculpt_color_grids &&
      (mmd.sculpt_color_grids_num != old_corners_num ||
       mmd.sculpt_color_level < 1 || mmd.sculpt_color_level > 13))
  {
    return false;
  }
  if (mmd.sculpt_face_set_grids &&
      (mmd.sculpt_face_set_grids_num != old_corners_num ||
       mmd.sculpt_face_set_level < 1 || mmd.sculpt_face_set_level > 13))
  {
    return false;
  }
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.level != mmd.totlvl || layer.grids_num != old_corners_num ||
        (old_corners_num > 0 && layer.disps == nullptr))
    {
      return false;
    }
    for (const int old_corner : new_to_old) {
      if (old_corner < 0) {
        continue;
      }
      const MDisps &grid = layer.disps[old_corner];
      if (grid.level != layer.level || grid.totdisp != area) {
        return false;
      }
    }
  }
  /* No recoverable validation failures remain. Stage one layer at a time to
   * limit peak memory while preserving layer identity, name, strength and flags. */
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    MDisps *remapped = new_to_old.is_empty() ? nullptr :
        MEM_new_array_zeroed<MDisps>(new_to_old.size(), "remapped sculpt layer grids");
    for (const int new_corner : new_to_old.index_range()) {
      MDisps &dst = remapped[new_corner];
      dst.level = layer.level;
      dst.totdisp = area;
      const int old_corner = new_to_old[new_corner];
      if (old_corner >= 0) {
        /* Transfer ownership, including implicit zero grids, without duplicating high-res
         * payloads. The one-to-one map above ensures each payload is moved only once. */
        dst = layer.disps[old_corner];
        layer.disps[old_corner] = {};
      }
    }
    /* free_layer() also frees L0 deltas; those were remapped separately and must survive. */
    float (*base_deltas)[3] = layer.base_deltas;
    const int base_verts_num = layer.base_verts_num;
    layer.base_deltas = nullptr;
    free_layer(layer);
    layer.base_deltas = base_deltas;
    layer.base_verts_num = base_verts_num;
    layer.disps = remapped;
    layer.grids_num = int(new_to_old.size());
  }
  if (mmd.sculpt_color_grids) {
    const int stored_level = mmd.sculpt_color_level;
    MultiresColorGrid *remapped = new_to_old.is_empty() ? nullptr :
        MEM_new_array_zeroed<MultiresColorGrid>(new_to_old.size(), "remapped polypaint grids");
    for (const int new_corner : new_to_old.index_range()) {
      const int old_corner = new_to_old[new_corner];
      if (old_corner >= 0) {
        remapped[new_corner] = mmd.sculpt_color_grids[old_corner];
        mmd.sculpt_color_grids[old_corner] = {};
      }
    }
    polypaint_clear(mmd);
    mmd.sculpt_color_grids = remapped;
    mmd.sculpt_color_grids_num = int(new_to_old.size());
    mmd.sculpt_color_level = stored_level;
  }
  for (MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
    if (!attribute.grids) {
      continue;
    }
    MultiresColorGrid *remapped = new_to_old.is_empty() ? nullptr :
        MEM_new_array_zeroed<MultiresColorGrid>(new_to_old.size(), "remapped Paint attribute");
    for (const int new_corner : new_to_old.index_range()) {
      const int old_corner = new_to_old[new_corner];
      if (old_corner >= 0) {
        remapped[new_corner] = attribute.grids[old_corner];
        attribute.grids[old_corner] = {};
      }
    }
    for (int i = 0; i < attribute.grids_num; i++) {
      MEM_SAFE_DELETE(attribute.grids[i].colors);
    }
    MEM_delete(attribute.grids);
    attribute.grids = remapped;
    attribute.grids_num = int(new_to_old.size());
  }
  if (mmd.sculpt_face_set_grids) {
    MultiresFaceSetGrid *remapped = new_to_old.is_empty() ? nullptr :
        MEM_new_array<MultiresFaceSetGrid>(new_to_old.size(),
                                                   "remapped multires face set grids");
    for (const int new_corner : new_to_old.index_range()) {
      const int old_corner = new_to_old[new_corner];
      if (old_corner >= 0) {
        remapped[new_corner] = mmd.sculpt_face_set_grids[old_corner];
        mmd.sculpt_face_set_grids[old_corner] = {};
      }
      else {
        remapped[new_corner].level = mmd.sculpt_face_set_level;
        remapped[new_corner].uniform_id = 1;
      }
    }
    const int stored_level = mmd.sculpt_face_set_level;
    faceset_clear(mmd);
    mmd.sculpt_face_set_grids = remapped;
    mmd.sculpt_face_set_grids_num = int(new_to_old.size());
    mmd.sculpt_face_set_level = stored_level;
  }
  mmd.sculpt_layer_recording = 0;
  return true;
}

/* A mesh system property survives BMesh replacement, including deleting all vertices.
 * It is a high-water mark, not a grid index. Older full memfile states restore it
 * together with their modifier data; Edit snapshots cannot accidentally recycle it. */
static constexpr char corner_counter_property[] = "sculpt_layer_corner_id_counter";

static int corner_id_counter(Mesh &mesh)
{
  IDProperty *property = IDP_GetPropertyFromGroup_null(
      IDP_ID_system_properties_get(&mesh.id), corner_counter_property);
  if (!property) {
    return 0;
  }
  return property->type == IDP_INT ? std::max(0, IDP_int_get(property)) : -1;
}

static void corner_id_counter_set(Mesh &mesh, const int value)
{
  IDProperty *group = IDP_ID_system_properties_ensure(&mesh.id);
  IDProperty *property = IDP_GetPropertyFromGroup_null(group, corner_counter_property);
  if (!property) {
    IDP_AddToGroup(group, IDP_NewInt(value, corner_counter_property));
  }
  else {
    IDP_int_set(property, std::max(IDP_int_get(property), value));
  }
}

void ensure_corner_ids(MultiresModifierData &mmd, Mesh &mesh)
{
  if (mmd.sculpt_layers.is_empty() && !mmd.sculpt_color_grids &&
      mmd.sculpt_color_attributes.is_empty() && !mmd.sculpt_face_set_grids) {
    return;
  }
  /* An existing attribute belongs to the current topology. Keep it until a
   * mismatch is consumed by remap_after_topology_edit(). */
  if (mesh.attributes().contains(corner_id_attribute)) {
    return;
  }
  int stored_corners = 0;
  if (!mmd.sculpt_layers.is_empty()) {
    stored_corners = static_cast<const MultiresSculptLayer *>(mmd.sculpt_layers.first)->grids_num;
  }
  else if (mmd.sculpt_face_set_grids) {
    stored_corners = mmd.sculpt_face_set_grids_num;
  }
  else if (mmd.sculpt_color_grids) {
    stored_corners = mmd.sculpt_color_grids_num;
  }
  else {
    stored_corners = mesh.corners_num;
    for (const MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
      if (attribute.grids) {
        stored_corners = attribute.grids_num;
        break;
      }
    }
  }
  if (stored_corners != mesh.corners_num) {
    return;
  }
  const int id_base = corner_id_counter(mesh);
  if (id_base < 0 || id_base > INT_MAX - mesh.corners_num) {
    return;
  }
  SpanAttributeWriter<int> ids = mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
      corner_id_attribute, AttrDomain::Corner);
  for (const int corner : ids.span.index_range()) {
    ids.span[corner] = id_base + corner + 1;
  }
  corner_id_counter_set(mesh, id_base + mesh.corners_num);
  ids.finish();
  SpanAttributeWriter<int> previous = mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
      corner_prev_attribute, AttrDomain::Corner);
  SpanAttributeWriter<int> next = mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
      corner_next_attribute, AttrDomain::Corner);
  SpanAttributeWriter<int> sizes = mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
      corner_face_size_attribute, AttrDomain::Corner);
  const OffsetIndices faces = mesh.faces();
  for (const int face_i : faces.index_range()) {
    const IndexRange face = faces[face_i];
    for (const int corner : face) {
      previous.span[corner] = id_base + (corner == face.start() ? face.last() : corner - 1) + 1;
      next.span[corner] = id_base + (corner == face.last() ? face.start() : corner + 1) + 1;
      sizes.span[corner] = face.size();
    }
  }
  previous.finish();
  next.finish();
  sizes.finish();
}

/* BMesh owns native Multires interpolation. Keep valid results, even when no Sculpt Layer
 * identity can be recovered. New layer deltas and native base displacement are independent. */
static bool initialize_missing_base_grid(MDisps &grid, const int level, const int area)
{
  if (grid.disps && grid.level == level && grid.totdisp == area) {
    return false;
  }
  MEM_SAFE_DELETE(grid.disps);
  MEM_SAFE_DELETE(grid.hidden);
  grid.level = level;
  grid.totdisp = area;
  grid.disps = MEM_new_array_zeroed<float[3]>(area, "new multires base corner");
  return true;
}

bool remap_after_topology_edit(MultiresModifierData &mmd,
                              Mesh &mesh,
                              BitVector<> *r_reset_corners,
                              const Span<int> old_corner_ids)
{
  if (r_reset_corners) {
    r_reset_corners->clear();
  }
  if (!base_remap_after_topology_edit(mmd, mesh)) {
    return false;
  }
  if (mmd.totlvl == 0) {
    return compatible(mmd, mesh);
  }
  if (mmd.sculpt_layers.is_empty() && !mmd.sculpt_color_grids &&
      mmd.sculpt_color_attributes.is_empty() && !mmd.sculpt_face_set_grids) {
    return true;
  }
  int old_corners_num = 0;
  if (!mmd.sculpt_layers.is_empty()) {
    old_corners_num = static_cast<const MultiresSculptLayer *>(mmd.sculpt_layers.first)->grids_num;
  }
  else if (mmd.sculpt_face_set_grids) {
    old_corners_num = mmd.sculpt_face_set_grids_num;
  }
  else {
    old_corners_num = mesh.corners_num;
    if (mmd.sculpt_color_grids) {
      old_corners_num = mmd.sculpt_color_grids_num;
    }
    else {
      for (const MultiresColorAttribute &attribute : mmd.sculpt_color_attributes) {
        if (attribute.grids) {
          old_corners_num = attribute.grids_num;
          break;
        }
      }
    }
  }
  ensure_corner_ids(mmd, mesh);
  const AttributeReader<int> ids_reader = mesh.attributes().lookup<int>(corner_id_attribute,
                                                                        AttrDomain::Corner);
  if (!ids_reader) {
    return false;
  }
  const VArraySpan<int> ids(ids_reader.varray);
  if (ids.size() != mesh.corners_num) {
    return false;
  }
  const AttributeReader<int> previous_reader = mesh.attributes().lookup<int>(
      corner_prev_attribute, AttrDomain::Corner);
  const AttributeReader<int> next_reader = mesh.attributes().lookup<int>(
      corner_next_attribute, AttrDomain::Corner);
  const AttributeReader<int> size_reader = mesh.attributes().lookup<int>(
      corner_face_size_attribute, AttrDomain::Corner);
  if (!previous_reader || !next_reader || !size_reader) {
    return false;
  }
  const VArraySpan<int> previous(previous_reader.varray);
  const VArraySpan<int> next(next_reader.varray);
  const VArraySpan<int> sizes(size_reader.varray);
  /* Grid storage follows the OLD mesh array order, not numeric ID order. */
  Array<int> canonical_ids;
  Span<int> storage_ids = old_corner_ids;
  if (storage_ids.is_empty() && old_corners_num > 0) {
    canonical_ids.reinitialize(old_corners_num);
    for (const int i : canonical_ids.index_range()) {
      canonical_ids[i] = i + 1;
    }
    storage_ids = canonical_ids.as_span();
  }
  if (storage_ids.size() != old_corners_num) {
    return false;
  }
  Array<int> counts(old_corners_num, 0);
  Array<int> new_to_old(mesh.corners_num, -1);
  int max_old_id = 0;
  const int high_water_mark = corner_id_counter(mesh);
  if (high_water_mark < 0) {
    return false;
  }
  if (!detail::corner_identity_lookup(storage_ids.data(),
                                     old_corners_num,
                                     ids.data(),
                                     mesh.corners_num,
                                     new_to_old.data(),
                                     counts.data(),
                                     max_old_id))
  {
    return false;
  }
  max_old_id = std::max(max_old_id, high_water_mark);
  const OffsetIndices faces = mesh.faces();
  bool identity = old_corners_num == mesh.corners_num;
  for (const int face_i : faces.index_range()) {
    const IndexRange face = faces[face_i];
    for (const int corner : face) {
      const int id = new_to_old[corner];
      const int prev_corner = corner == face.start() ? face.last() : corner - 1;
      const int next_corner = corner == face.last() ? face.start() : corner + 1;
      /* A retained loop ID alone cannot establish grid orientation after Dissolve, Flip
       * or rebuilding a face. Preserve only an unchanged cyclic neighborhood. */
      if (id >= 0 && id < old_corners_num && counts[id] == 1 &&
          sizes[corner] == face.size() && previous[corner] == ids[prev_corner] &&
          next[corner] == ids[next_corner])
      {
        new_to_old[corner] = id;
      }
      else {
        new_to_old[corner] = -1;
      }
      identity &= new_to_old[corner] == corner;
    }
  }
  if (identity) {
    return true;
  }

  /* Surviving identities stay stable across remaps. Allocate fresh IDs only for
   * new/ambiguous corners; never recycle old array indices as identities. */
  int new_ids_num = 0;
  for (const int old_corner : new_to_old) {
    new_ids_num += old_corner < 0;
  }
  if (max_old_id > INT_MAX - new_ids_num) {
    return false;
  }
  Array<int> stable_ids(mesh.corners_num);
  int next_id = max_old_id;
  for (const int corner : new_to_old.index_range()) {
    stable_ids[corner] = new_to_old[corner] >= 0 ? storage_ids[new_to_old[corner]] : ++next_id;
  }

  /* Native Multires displacement is stored directly on the current mesh
   * corners. BMesh preserves CD_MDISPS for surviving loops, while newly
   * created loops have empty/default grids. Validate surviving grids before
   * touching the Sculpt Layer stack; after a successful layer remap, allocate
   * zero displacement for genuinely new base corners. */
  const int side = subdiv::grid_size_from_level(mmd.totlvl);
  const int area = side * side;
  MDisps *base = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh.corner_data, CD_MDISPS, mesh.corners_num));
  if (base == nullptr) {
    return false;
  }
  for (const int new_corner : new_to_old.index_range()) {
    if (new_to_old[new_corner] < 0) {
      continue;
    }
    const MDisps &grid = base[new_corner];
    if (grid.level != mmd.totlvl || grid.totdisp != area || grid.disps == nullptr) {
      return false;
    }
  }

  if (!remap_surviving_corners(mmd, old_corners_num, new_to_old)) {
    return false;
  }

  if (r_reset_corners) {
    r_reset_corners->resize(mesh.corners_num, false);
  }
  for (const int new_corner : new_to_old.index_range()) {
    if (new_to_old[new_corner] >= 0) {
      continue;
    }
    MDisps &grid = base[new_corner];
    const bool initialized = initialize_missing_base_grid(grid, mmd.totlvl, area);
    if (r_reset_corners && initialized) {
      (*r_reset_corners)[new_corner].set();
    }
  }

  corner_id_counter_set(mesh, next_id);
  SpanAttributeWriter<int> published_ids =
      mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
          corner_id_attribute, AttrDomain::Corner);
  published_ids.span.copy_from(stable_ids);
  published_ids.finish();
  SpanAttributeWriter<int> published_previous =
      mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
          corner_prev_attribute, AttrDomain::Corner);
  SpanAttributeWriter<int> published_next =
      mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
          corner_next_attribute, AttrDomain::Corner);
  SpanAttributeWriter<int> published_sizes =
      mesh.attributes_for_write().lookup_or_add_for_write_span<int>(
          corner_face_size_attribute, AttrDomain::Corner);
  for (const int face_i : faces.index_range()) {
    const IndexRange face = faces[face_i];
    for (const int corner : face) {
      published_previous.span[corner] =
          stable_ids[corner == face.start() ? face.last() : corner - 1];
      published_next.span[corner] =
          stable_ids[corner == face.last() ? face.start() : corner + 1];
      published_sizes.span[corner] = face.size();
    }
  }
  published_previous.finish();
  published_next.finish();
  published_sizes.finish();
  return true;
}

MultiresSculptLayer *add(MultiresModifierData &mmd,
                         const char *name,
                         const int level,
                         const int grids_num)
{
  if (level < 0 || level > 13 || (level > 0 && grids_num <= 0)) {
    return nullptr;
  }
  mmd.sculpt_layer_recording = 0;
  auto *layer = MEM_new<MultiresSculptLayer>("multires sculpt layer");
  BLI_strncpy(layer->name, name ? name : "Layer", sizeof(layer->name));
  layer->strength = 1.0f;
  layer->flag = MULTIRES_SCULPT_LAYER_VISIBLE;
  layer->group_id = 0;
  layer->group_flag = 0;
  layer->group_name[0] = '\0';
  allocate_layer_grids(*layer, level, grids_num);
  BLI_addtail(&mmd.sculpt_layers, layer);
  BLI_uniquename(&mmd.sculpt_layers,
                 layer,
                 "Layer",
                 '.',
                 offsetof(MultiresSculptLayer, name),
                 sizeof(layer->name));
  mmd.sculpt_layer_active = mmd.sculpt_layers.count() - 1;
  return layer;
}

void remove(MultiresModifierData &mmd, MultiresSculptLayer &layer)
{
  weight_hot_delta_cache_invalidate(&layer);
  const int index = BLI_findindex(&mmd.sculpt_layers, &layer);
  if (index < 0) {
    return;
  }
  mmd.sculpt_layer_recording = 0;
  BLI_remlink(&mmd.sculpt_layers, &layer);
  free_layer(layer);
  MEM_delete(&layer);
  mmd.sculpt_layer_active = mmd.sculpt_layers.is_empty() ? 0 :
                               std::clamp(mmd.sculpt_layer_active, 0, mmd.sculpt_layers.count() - 1);
}

MultiresSculptLayer *active(MultiresModifierData &mmd)
{
  if (mmd.sculpt_layers.is_empty()) {
    return nullptr;
  }
  const int index = std::clamp(mmd.sculpt_layer_active, 0, mmd.sculpt_layers.count() - 1);
  return static_cast<MultiresSculptLayer *>(BLI_findlink(&mmd.sculpt_layers, index));
}

const MultiresSculptLayer *active(const MultiresModifierData &mmd)
{
  return active(const_cast<MultiresModifierData &>(mmd));
}

void set_active(MultiresModifierData &mmd, const int index)
{
  mmd.sculpt_layer_recording = 0;
  if (mmd.sculpt_layers.is_empty()) {
    mmd.sculpt_layer_active = 0;
    return;
  }
  mmd.sculpt_layer_active = std::clamp(index, 0, mmd.sculpt_layers.count() - 1);
}

bool move(MultiresModifierData &mmd, const int from_index, const int to_index)
{
  const int count = mmd.sculpt_layers.count();
  if (count < 2 || from_index < 0 || from_index >= count || to_index < 0 ||
      to_index >= count || from_index == to_index)
  {
    return false;
  }

  /* Preserve the active layer by identity, not by index arithmetic. */
  MultiresSculptLayer *active_layer = active(mmd);
  MultiresSculptLayer *moving_layer = static_cast<MultiresSculptLayer *>(
      BLI_findlink(&mmd.sculpt_layers, from_index));
  if (moving_layer == nullptr ||
      !BLI_listbase_move_index(&mmd.sculpt_layers, from_index, to_index))
  {
    return false;
  }

  mmd.sculpt_layer_recording = 0;
  if (active_layer != nullptr) {
    const int active_index = BLI_findindex(&mmd.sculpt_layers, active_layer);
    mmd.sculpt_layer_active = active_index >= 0 ? active_index : 0;
  }
  else {
    mmd.sculpt_layer_active = 0;
  }
  return true;
}


static int group_next_id(const MultiresModifierData &mmd)
{
  int max_id = 0;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    max_id = std::max(max_id, layer.group_id);
  }
  return max_id + 1;
}

static void group_assign(MultiresSculptLayer &layer,
                         const int group_id,
                         const char *group_name,
                         const int group_flag)
{
  layer.group_id = group_id;
  layer.group_flag = group_flag;
  BLI_strncpy(layer.group_name, group_name ? group_name : "", sizeof(layer.group_name));

  /* Joining a hidden group must preserve the layer's own visibility for restore. */
  if (group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN) {
    if (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) {
      layer.flag |= MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
    }
    layer.flag &= ~MULTIRES_SCULPT_LAYER_VISIBLE;
  }
}

bool group_with_below(MultiresModifierData &mmd)
{
  MultiresSculptLayer *src = active(mmd);
  if (src == nullptr || src->next == nullptr) {
    return false;
  }
  MultiresSculptLayer *below = static_cast<MultiresSculptLayer *>(src->next);

  int target_id = 0;
  int target_flag = 0;
  char target_name[64] = "";

  if (src->group_id != 0) {
    target_id = src->group_id;
    target_flag = src->group_flag;
    BLI_strncpy(target_name, src->group_name, sizeof(target_name));
  }
  else if (below->group_id != 0) {
    target_id = below->group_id;
    target_flag = below->group_flag;
    BLI_strncpy(target_name, below->group_name, sizeof(target_name));
  }
  else {
    target_id = group_next_id(mmd);
    BLI_snprintf(target_name, sizeof(target_name), "Group %d", target_id);
  }

  const int src_old = src->group_id;
  const int below_old = below->group_id;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if ((src_old != 0 && layer.group_id == src_old) ||
        (below_old != 0 && layer.group_id == below_old))
    {
      group_assign(layer, target_id, target_name, target_flag);
    }
  }
  if (src_old == 0) {
    group_assign(*src, target_id, target_name, target_flag);
  }
  if (below_old == 0) {
    group_assign(*below, target_id, target_name, target_flag);
  }
  return true;
}


bool group_create_from_active(MultiresModifierData &mmd)
{
  MultiresSculptLayer *layer = active(mmd);
  if (layer == nullptr) {
    return false;
  }

  if (layer->group_id != 0 &&
      (layer->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN))
  {
    if (layer->flag & MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE) {
      layer->flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
    }
    layer->flag &= ~MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
  }

  const int group_id = group_next_id(mmd);
  char name[64];
  BLI_snprintf(name, sizeof(name), "Group %d", group_id);

  layer->group_id = group_id;
  layer->group_flag = 0;
  BLI_strncpy(layer->group_name, name, sizeof(layer->group_name));
  mmd.sculpt_layer_recording = 0;
  return true;
}

bool group_add_active(MultiresModifierData &mmd, const int group_id)
{
  if (group_id <= 0) {
    return false;
  }

  MultiresSculptLayer *layer = active(mmd);
  if (layer == nullptr) {
    return false;
  }

  MultiresSculptLayer *target_first = nullptr;
  MultiresSculptLayer *target_last = nullptr;
  for (MultiresSculptLayer &candidate : mmd.sculpt_layers) {
    if (candidate.group_id == group_id) {
      if (target_first == nullptr) {
        target_first = &candidate;
      }
      target_last = &candidate;
    }
  }
  if (target_first == nullptr || target_last == nullptr) {
    return false;
  }
  if (layer->group_id == group_id) {
    return true;
  }

  if (layer->group_id != 0 &&
      (layer->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN))
  {
    if (layer->flag & MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE) {
      layer->flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
    }
    layer->flag &= ~MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
  }

  const int target_flag = target_first->group_flag;
  char target_name[64];
  BLI_strncpy(target_name, target_first->group_name, sizeof(target_name));

  /* Keep folders physically contiguous. Additive layer order does not change
   * the evaluated surface, so this is an organizational reorder only. */
  BLI_remlink(&mmd.sculpt_layers, layer);
  BLI_insertlinkafter(&mmd.sculpt_layers, target_last, layer);
  group_assign(*layer, group_id, target_name, target_flag);

  mmd.sculpt_layer_active = BLI_findindex(&mmd.sculpt_layers, layer);
  mmd.sculpt_layer_recording = 0;
  return true;
}

bool group_merge(MultiresModifierData &mmd, const int group_id)
{
  if (group_id <= 0) {
    return false;
  }

  MultiresSculptLayer *first = nullptr;
  MultiresSculptLayer *last = nullptr;
  int count = 0;
  bool left_group = false;

  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.group_id == group_id) {
      if (left_group) {
        return false;
      }
      if (first == nullptr) {
        first = &layer;
      }
      last = &layer;
      count++;
    }
    else if (first != nullptr) {
      left_group = true;
    }
  }

  if (first == nullptr || last == nullptr || count < 2) {
    return false;
  }
  if (first->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN) {
    return false;
  }

  const int level = first->level;
  const int grids_num = first->grids_num;
  int base_count = 0;

  for (MultiresSculptLayer *layer = first;;
       layer = static_cast<MultiresSculptLayer *>(layer->next))
  {
    if (layer == nullptr || layer->group_id != group_id ||
        !(layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
        layer->level != level || layer->grids_num != grids_num ||
        (level > 0 && layer->disps == nullptr))
    {
      return false;
    }

    if (layer->base_verts_num != 0) {
      if (base_count == 0) {
        base_count = layer->base_verts_num;
      }
      else if (base_count != layer->base_verts_num) {
        return false;
      }
    }

    if (level > 0) {
      for (int grid = 0; grid < grids_num; grid++) {
        if (layer->disps[grid].level != level ||
            first->disps[grid].totdisp != layer->disps[grid].totdisp)
        {
          return false;
        }
      }
    }

    if (layer == last) {
      break;
    }
  }

  char merged_name[64];
  BLI_strncpy(merged_name,
              first->group_name[0] ? first->group_name : "Merged Group",
              sizeof(merged_name));

  mmd.sculpt_layer_active = BLI_findindex(&mmd.sculpt_layers, first);

  /* merge_down() is already sparse-safe and targets src->next. Each merge
   * leaves the destination active, so the active layer walks down the folder. */
  while (true) {
    MultiresSculptLayer *src = active(mmd);
    if (src == nullptr || src->group_id != group_id) {
      return false;
    }
    MultiresSculptLayer *next =
        src->next ? static_cast<MultiresSculptLayer *>(src->next) : nullptr;
    if (next == nullptr || next->group_id != group_id) {
      break;
    }
    if (!merge_down(mmd)) {
      return false;
    }
  }

  MultiresSculptLayer *result = active(mmd);
  if (result == nullptr) {
    return false;
  }
  BLI_strncpy(result->name, merged_name, sizeof(result->name));
  result->group_id = 0;
  result->group_flag = 0;
  result->group_name[0] = '\0';
  result->flag &= ~MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
  mmd.sculpt_layer_recording = 0;
  return true;
}

bool group_remove_active(MultiresModifierData &mmd)
{
  MultiresSculptLayer *layer = active(mmd);
  if (layer == nullptr || layer->group_id == 0) {
    return false;
  }

  if (layer->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN) {
    if (layer->flag & MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE) {
      layer->flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
    }
    layer->flag &= ~MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
  }

  layer->group_id = 0;
  layer->group_flag = 0;
  layer->group_name[0] = '\0';
  return true;
}

bool group_toggle_visibility(MultiresModifierData &mmd, const int group_id)
{
  if (group_id <= 0) {
    return false;
  }

  MultiresSculptLayer *first = nullptr;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.group_id == group_id) {
      first = &layer;
      break;
    }
  }
  if (first == nullptr) {
    return false;
  }

  const bool hide = (first->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN) == 0;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.group_id != group_id) {
      continue;
    }

    if (hide) {
      if (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) {
        layer.flag |= MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
      }
      else {
        layer.flag &= ~MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
      }
      layer.flag &= ~MULTIRES_SCULPT_LAYER_VISIBLE;
      layer.group_flag |= MULTIRES_SCULPT_LAYER_GROUP_HIDDEN;
    }
    else {
      if (layer.flag & MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE) {
        layer.flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
      }
      layer.flag &= ~MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE;
      layer.group_flag &= ~MULTIRES_SCULPT_LAYER_GROUP_HIDDEN;
    }
  }
  mmd.sculpt_layer_recording = 0;
  return true;
}

bool group_toggle_collapsed(MultiresModifierData &mmd, const int group_id)
{
  if (group_id <= 0) {
    return false;
  }

  MultiresSculptLayer *first = nullptr;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.group_id == group_id) {
      first = &layer;
      break;
    }
  }
  if (first == nullptr) {
    return false;
  }

  const bool collapse =
      (first->group_flag & MULTIRES_SCULPT_LAYER_GROUP_COLLAPSED) == 0;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.group_id != group_id) {
      continue;
    }
    if (collapse) {
      layer.group_flag |= MULTIRES_SCULPT_LAYER_GROUP_COLLAPSED;
    }
    else {
      layer.group_flag &= ~MULTIRES_SCULPT_LAYER_GROUP_COLLAPSED;
    }
  }
  return true;
}

MultiresSculptLayer *previous(MultiresModifierData & /*mmd*/, const MultiresSculptLayer &layer)
{
  return layer.prev ? static_cast<MultiresSculptLayer *>(layer.prev) : nullptr;
}

bool merge_down(MultiresModifierData &mmd)
{
  weight_hot_delta_cache_invalidate();
  MultiresSculptLayer *src = active(mmd);
  if (src == nullptr || src->next == nullptr) {
    return false;
  }

  MultiresSculptLayer *dst = static_cast<MultiresSculptLayer *>(src->next);
  if (dst == nullptr || src->level != dst->level || src->grids_num != dst->grids_num ||
      (src->level > 0 && (src->disps == nullptr || dst->disps == nullptr)) ||
      !(src->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
      !(dst->flag & MULTIRES_SCULPT_LAYER_VISIBLE))
  {
    return false;
  }

  /* V4.5 sparse Sculpt Layers: a null per-grid payload is a valid all-zero grid.
   * Only descriptor incompatibility is an error. */
  for (int grid = 0; grid < src->grids_num; grid++) {
    const MDisps &src_grid = src->disps[grid];
    const MDisps &dst_grid = dst->disps[grid];
    if (src_grid.level != src->level || dst_grid.level != dst->level ||
        src_grid.totdisp != dst_grid.totdisp)
    {
      return false;
    }
  }

  const int base_count = std::max(src->base_verts_num, dst->base_verts_num);
  if ((src->base_verts_num && src->base_verts_num != base_count) ||
      (dst->base_verts_num && dst->base_verts_num != base_count))
  {
    return false;
  }

  Array<float3> merged_base(base_count, float3(0));
  std::vector<base_math::LayerView> base_layers;
  if (src->base_verts_num) {
    base_layers.push_back({src->base_deltas, base_count, src->strength, true});
  }
  if (dst->base_verts_num) {
    base_layers.push_back({dst->base_deltas, base_count, dst->strength, true});
  }
  if (!base_math::compose(reinterpret_cast<const float (*)[3]>(merged_base.data()),
                          base_count,
                          base_layers,
                          reinterpret_cast<float (*)[3]>(merged_base.data())) ||
      !base_deltas_ensure(*dst, base_count))
  {
    return false;
  }
  if (base_count) {
    memcpy(dst->base_deltas, merged_base.data(), sizeof(float3) * base_count);
  }

  /* Merge effective contributions and normalize the destination to strength 1.0.
   * Sparse zero grids remain unallocated whenever possible. */
  const float src_scale = src->strength;
  const float dst_scale = dst->strength;

  for (int grid = 0; grid < src->grids_num; grid++) {
    const MDisps &src_grid = src->disps[grid];
    MDisps &dst_grid = dst->disps[grid];

    const bool src_has_data = src_grid.disps != nullptr;
    const bool dst_has_data = dst_grid.disps != nullptr;

    if (!src_has_data && !dst_has_data) {
      continue;
    }

    float (*dst_data)[3] = dst_grid.disps;
    if (dst_data == nullptr) {
      dst_data = grid_ensure(*dst, grid);
      if (dst_data == nullptr) {
        return false;
      }
    }

    const int count = dst_grid.totdisp;

    if (!src_has_data) {
      for (int i = 0; i < count; i++) {
        for (int axis = 0; axis < 3; axis++) {
          dst_data[i][axis] *= dst_scale;
        }
      }
      continue;
    }

    if (!dst_has_data) {
      for (int i = 0; i < count; i++) {
        for (int axis = 0; axis < 3; axis++) {
          dst_data[i][axis] = src_grid.disps[i][axis] * src_scale;
        }
      }
      continue;
    }

    for (int i = 0; i < count; i++) {
      for (int axis = 0; axis < 3; axis++) {
        dst_data[i][axis] =
            dst_data[i][axis] * dst_scale +
            src_grid.disps[i][axis] * src_scale;
      }
    }
  }

  mmd.sculpt_layer_recording = 0;
  dst->strength = 1.0f;

  const int dst_index = BLI_findindex(&mmd.sculpt_layers, dst);
  BLI_remlink(&mmd.sculpt_layers, src);
  free_layer(*src);
  MEM_delete(src);
  mmd.sculpt_layer_active = dst_index;
  return true;
}

void flatten(MultiresModifierData &mmd)
{
  weight_hot_delta_cache_invalidate();
  if (mmd.sculpt_layers.is_empty()) {
    return;
  }
  MultiresSculptLayer *dst = static_cast<MultiresSculptLayer *>(mmd.sculpt_layers.first);
  if (dst == nullptr) {
    return;
  }

  int base_count = 0;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    base_count = std::max(base_count, layer.base_verts_num);
  }
  Array<float3> flattened_base(base_count, float3(0));
  std::vector<base_math::LayerView> base_layers;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (layer.base_verts_num) {
      base_layers.push_back({layer.base_deltas, layer.base_verts_num, layer.strength,
                             bool(layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE)});
    }
  }
  if (!base_math::compose(reinterpret_cast<const float (*)[3]>(flattened_base.data()),
                            base_count, base_layers,
                            reinterpret_cast<float (*)[3]>(flattened_base.data())) ||
      !base_deltas_ensure(*dst, base_count)) {
    return;
  }
  if (base_count) {
    memcpy(dst->base_deltas, flattened_base.data(), sizeof(float3) * base_count);
  }

  /* Bake all effective layer contributions into the first layer. */
  for (int grid = 0; grid < dst->grids_num; grid++) {
    if (dst->disps == nullptr || dst->disps[grid].disps == nullptr) {
      continue;
    }
    for (int i = 0; i < dst->disps[grid].totdisp; i++) {
      for (int axis = 0; axis < 3; axis++) {
        dst->disps[grid].disps[i][axis] *= dst->strength;
      }
    }
  }
  for (MultiresSculptLayer *src = static_cast<MultiresSculptLayer *>(dst->next); src;) {
    MultiresSculptLayer *next = static_cast<MultiresSculptLayer *>(src->next);
    if (src->level == dst->level && src->grids_num == dst->grids_num && src->disps && dst->disps) {
      for (int grid = 0; grid < src->grids_num; grid++) {
        if (!src->disps[grid].disps || !dst->disps[grid].disps ||
            src->disps[grid].totdisp != dst->disps[grid].totdisp)
        {
          continue;
        }
        for (int i = 0; i < src->disps[grid].totdisp; i++) {
          for (int axis = 0; axis < 3; axis++) {
            dst->disps[grid].disps[i][axis] += src->disps[grid].disps[i][axis] * src->strength;
          }
        }
      }
    }
    BLI_remlink(&mmd.sculpt_layers, src);
    free_layer(*src);
    MEM_delete(src);
    src = next;
  }
  mmd.sculpt_layer_recording = 0;
  dst->strength = 1.0f;
  BLI_strncpy(dst->name, "Sculpt Layer", sizeof(dst->name));
  mmd.sculpt_layer_active = 0;
}

void set_solo(MultiresModifierData &mmd, const int index)
{
  mmd.sculpt_layer_recording = 0;
  int i = 0;
  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    set_visible(layer, i == index);
    i++;
  }
}

void set_visible(MultiresSculptLayer &layer, const bool visible)
{
  if (visible) {
    layer.flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
  }
  else {
    layer.flag &= ~MULTIRES_SCULPT_LAYER_VISIBLE;
  }
}

void clear(MultiresModifierData &mmd)
{
  /* Invalidate only the layer payloads this modifier actually owns. */
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    weight_hot_delta_cache_invalidate(&layer);
  }
  polypaint_attributes_clear(mmd);
  faceset_clear(mmd);
  mmd.sculpt_layer_recording = 0;
  while (!mmd.sculpt_layers.is_empty()) {
    MultiresSculptLayer &layer = *static_cast<MultiresSculptLayer *>(mmd.sculpt_layers.first);
    BLI_remlink(&mmd.sculpt_layers, &layer);
    free_layer(layer);
    MEM_delete(&layer);
  }
  mmd.sculpt_layer_active = 0;
}

static void copy_layer(MultiresSculptLayer &dst, const MultiresSculptLayer &src)
{
  BLI_strncpy(dst.name, src.name, sizeof(dst.name));
  dst.strength = src.strength;
  dst.flag = src.flag;
  dst.group_id = src.group_id;
  dst.group_flag = src.group_flag;
  BLI_strncpy(dst.group_name, src.group_name, sizeof(dst.group_name));
  dst.level = src.level;
  dst.base_verts_num = src.base_verts_num;
  dst.base_deltas = src.base_deltas ? MEM_dupalloc(src.base_deltas) : nullptr;
  dst.grids_num = 0;
  dst.disps = nullptr;
  if (src.disps == nullptr || src.grids_num == 0) {
    return;
  }
  dst.grids_num = src.grids_num;
  dst.disps = MEM_new_array_zeroed<MDisps>(dst.grids_num, "multires sculpt layer copy");
  for (int i = 0; i < dst.grids_num; i++) {
    dst.disps[i].totdisp = src.disps[i].totdisp;
    dst.disps[i].level = src.disps[i].level;
    if (src.disps[i].disps != nullptr) {
      dst.disps[i].disps = MEM_dupalloc(src.disps[i].disps);
    }
    if (src.disps[i].hidden != nullptr) {
      dst.disps[i].hidden = MEM_dupalloc(src.disps[i].hidden);
    }
  }
}

MultiresSculptLayer *duplicate(MultiresModifierData &mmd, const MultiresSculptLayer &source)
{
  mmd.sculpt_layer_recording = 0;
  auto *layer = MEM_new<MultiresSculptLayer>("multires sculpt layer duplicate");
  copy_layer(*layer, source);
  BLI_insertlinkafter(&mmd.sculpt_layers, const_cast<MultiresSculptLayer *>(&source), layer);
  BLI_uniquename(&mmd.sculpt_layers,
                 layer,
                 "Layer",
                 '.',
                 offsetof(MultiresSculptLayer, name),
                 sizeof(layer->name));
  mmd.sculpt_layer_active = BLI_findindex(&mmd.sculpt_layers, layer);
  return layer;
}

void copy(MultiresModifierData &dst, const MultiresModifierData &src)
{
  /* LAYER_WEIGHT_CACHE_SCOPE_FIX_V1: copy creates a new payload; source is unchanged. */
  /* The caller must make sure the destination does not own a previous layer list.
   * Do not call clear() here: after a generic modifier copy the destination list may
   * temporarily contain the source pointers. */
  dst.sculpt_layers.clear_no_delete();
  dst.sculpt_color_attributes.clear_no_delete();
  BLI_strncpy(dst.sculpt_color_attribute, src.sculpt_color_attribute,
              sizeof(dst.sculpt_color_attribute));
  for (const MultiresColorAttribute &source : src.sculpt_color_attributes) {
    auto *destination = MEM_new<MultiresColorAttribute>("Polypaint attribute copy");
    BLI_strncpy(destination->name, source.name, sizeof(destination->name));
    destination->grids_num = source.grids_num;
    destination->level = source.level;
    if (source.grids && source.grids_num > 0) {
      destination->grids = MEM_new_array_zeroed<MultiresColorGrid>(
          source.grids_num, "Polypaint attribute directory copy");
      for (int i = 0; i < source.grids_num; i++) {
        destination->grids[i] = source.grids[i];
        destination->grids[i].colors = source.grids[i].colors ?
            MEM_dupalloc(source.grids[i].colors) : nullptr;
      }
    }
    BLI_addtail(&dst.sculpt_color_attributes, destination);
  }
  dst.sculpt_color_grids = nullptr;
  dst.sculpt_color_grids_num = 0;
  dst.sculpt_color_level = 0;
  dst.sculpt_face_set_grids = nullptr;
  dst.sculpt_face_set_grids_num = 0;
  dst.sculpt_face_set_level = 0;
  dst.sculpt_layer_active = src.sculpt_layer_active;
  dst.sculpt_layer_recording = src.sculpt_layer_recording;
  for (const MultiresSculptLayer &src_layer : src.sculpt_layers) {
    auto *dst_layer = MEM_new<MultiresSculptLayer>("multires sculpt layer copy");
    copy_layer(*dst_layer, src_layer);
    BLI_addtail(&dst.sculpt_layers, dst_layer);
  }
  if (src.sculpt_color_grids && src.sculpt_color_grids_num > 0) {
    dst.sculpt_color_grids = MEM_new_array_zeroed<MultiresColorGrid>(
        src.sculpt_color_grids_num, "multires polypaint copy");
    dst.sculpt_color_grids_num = src.sculpt_color_grids_num;
    dst.sculpt_color_level = src.sculpt_color_level;
    for (int i = 0; i < src.sculpt_color_grids_num; i++) {
      dst.sculpt_color_grids[i].totcolor = src.sculpt_color_grids[i].totcolor;
      dst.sculpt_color_grids[i].level = src.sculpt_color_grids[i].level;
      if (src.sculpt_color_grids[i].colors) {
        dst.sculpt_color_grids[i].colors = MEM_dupalloc(src.sculpt_color_grids[i].colors);
      }
    }
  }
  if (src.sculpt_face_set_grids && src.sculpt_face_set_grids_num > 0) {
    dst.sculpt_face_set_grids = MEM_new_array<MultiresFaceSetGrid>(
        src.sculpt_face_set_grids_num, "multires face set copy");
    dst.sculpt_face_set_grids_num = src.sculpt_face_set_grids_num;
    dst.sculpt_face_set_level = src.sculpt_face_set_level;
    for (int i = 0; i < src.sculpt_face_set_grids_num; i++) {
      dst.sculpt_face_set_grids[i].totid = src.sculpt_face_set_grids[i].totid;
      dst.sculpt_face_set_grids[i].level = src.sculpt_face_set_grids[i].level;
      dst.sculpt_face_set_grids[i].uniform_id = src.sculpt_face_set_grids[i].uniform_id;
      if (src.sculpt_face_set_grids[i].ids) {
        dst.sculpt_face_set_grids[i].ids = MEM_dupalloc(src.sculpt_face_set_grids[i].ids);
      }
    }
  }
  dst.sculpt_layer_active = dst.sculpt_layers.is_empty() ? 0 :
                               std::clamp(dst.sculpt_layer_active, 0, dst.sculpt_layers.count() - 1);
}

bool bake_shrinkwrap_to_active(Depsgraph *depsgraph,
                              Object &object,
                              const bool respect_mask,
                              bool &r_used_mask)
{
  weight_hot_delta_cache_invalidate();
  r_used_mask = false;
  auto *mmd = reinterpret_cast<MultiresModifierData *>(
      BKE_modifiers_findby_type(&object, eModifierType_Multires));
  Mesh *mesh = BKE_mesh_from_object(&object);
  if (!depsgraph || !mmd || !mesh || !mmd->sculpt_shrinkwrap_target ||
      mmd->sculpt_shrinkwrap_strength <= 0.0f) {
    return false;
  }
  MultiresSculptLayer *layer = active(*mmd);
  if (!layer || !(layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
      !std::isfinite(layer->strength) || std::abs(layer->strength) < 1e-8f) {
    return false;
  }
  MultiresModifierData settings = dna::shallow_copy(*mmd);
  settings.sculpt_shrinkwrap_enabled = 1;
  Mesh *cage = BKE_mesh_copy_for_eval(*mesh);
  if (!base_deltas_apply(*mmd, cage->vert_positions_for_write())) {
    BKE_id_free(nullptr, cage);
    return false;
  }
  cage->tag_positions_changed();
  if (mmd->sculptlvl == 0 || mmd->totlvl == 0) {
    const Array<float3> original = cage->vert_positions();
    if (!shrinkwrap_mesh(depsgraph, object, settings, *cage, respect_mask)) {
      BKE_id_free(nullptr, cage);
      return false;
    }
    const VArray<float> masks = mesh->attributes().lookup_or_default<float>(
        ".sculpt_mask", AttrDomain::Point, 0.0f).varray;
    Array<float3> translations(mesh->verts_num);
    Array<int> indices(mesh->verts_num);
    for (const int i : translations.index_range()) {
      translations[i] = cage->vert_positions()[i] - original[i];
      indices[i] = i;
      r_used_mask |= respect_mask && masks[i] > 0.0f;
    }
    BKE_id_free(nullptr, cage);
    if (!base_deltas_ensure(*layer, mesh->verts_num) ||
        !base_deltas_record(*layer, indices, translations)) {
      return false;
    }
    base_ensure_vertex_ids(*mesh);
    return true;
  }
  if (!layer->disps || layer->grids_num != mesh->corners_num || layer->level != mmd->totlvl) {
    BKE_id_free(nullptr, cage);
    return false;
  }
  const int side = subdiv::grid_size_from_level(layer->level);

  /* Shrinkwrap bake is a global operation: any Multires grid can receive
   * displacement. Sparse Sculpt Layers keep untouched grid payloads null,
   * so materialize the active layer before writing the baked result. */
  for (const int grid : IndexRange(layer->grids_num)) {
    if (layer->disps[grid].totdisp != side * side ||
        grid_ensure(*layer, grid) == nullptr)
    {
      BKE_id_free(nullptr, cage);
      return false;
    }
  }
  subdiv::Settings subdiv_settings;
  BKE_multires_subdiv_settings_init(&subdiv_settings, mmd);
  subdiv::Subdiv *subdiv = subdiv::new_from_mesh(&subdiv_settings, cage);
  if (!subdiv) {
    BKE_id_free(nullptr, cage);
    return false;
  }
  subdiv::displacement_attach_from_multires(
      subdiv, cage, mmd, multires_texture_mapping_matrix(*mmd, object));
  SubdivToCCGSettings ccg_settings{};
  ccg_settings.resolution = (1 << mmd->totlvl) + 1;
  ccg_settings.need_normal = true;
  ccg_settings.need_mask = respect_mask &&
      CustomData_has_layer(&mesh->corner_data, CD_GRID_PAINT_MASK);
  if (!subdiv::eval_begin_from_mesh(subdiv, cage, subdiv::SUBDIV_EVALUATOR_TYPE_CPU)) {
    subdiv::free(subdiv);
    BKE_id_free(nullptr, cage);
    return false;
  }
  SubdivCCGMaskEvaluator mask_evaluator{};
  const bool has_mask = ccg_settings.need_mask &&
      BKE_subdiv_ccg_mask_init_from_paint(&mask_evaluator, cage);
  std::unique_ptr<SubdivCCG> ccg_owner = BKE_subdiv_to_ccg(
      *subdiv, ccg_settings, *cage, has_mask ? &mask_evaluator : nullptr);
  if (has_mask) {
    mask_evaluator.free(&mask_evaluator);
  }
  /* BKE_subdiv_to_ccg takes ownership of subdiv even when grid evaluation fails. */
  if (!ccg_owner) {
    BKE_id_free(nullptr, cage);
    return false;
  }
  SubdivCCG &ccg = *ccg_owner;
  Array<float3> deltas = ccg.positions;
  bool success = shrinkwrap_ccg(depsgraph, object, settings, *cage, ccg, respect_mask);
  MultiresReshapeContext context{};
  if (success) {
    success = multires_reshape_context_create_from_ccg(&context, &ccg, mesh, mmd->totlvl);
  }
  if (success) {
    const float inv = 1.0f / float(side - 1);
    for (const int grid : IndexRange(layer->grids_num)) {
      for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
          const int i = grid * side * side + y * side + x;
          const GridCoord coord{grid, float(x)*inv, float(y)*inv};
          float3 P;
          float3x3 tangent;
          multires_reshape_evaluate_base_mesh_limit_at_grid(&context, &coord, P, tangent);
          const float3 movement = ccg.positions[i] - deltas[i];
          deltas[i] = std::abs(math::determinant(tangent)) < 1e-12f ? float3(0.0f) :
              math::invert(tangent) * (movement / layer->strength);
          for (int axis = 0; axis < 3; axis++) {
            success &= std::isfinite(layer->disps[grid].disps[y*side+x][axis] + deltas[i][axis]);
          }
          r_used_mask |= respect_mask && !ccg.masks.is_empty() && ccg.masks[i] > 0.0f;
        }
      }
    }
    multires_reshape_context_free(&context);
  }
  ccg_owner.reset();
  BKE_id_free(nullptr, cage);
  if (!success) {
    return false;
  }
  for (const int grid : IndexRange(layer->grids_num)) {
    for (int i = 0; i < side * side; i++) {
      for (int axis = 0; axis < 3; axis++) {
        layer->disps[grid].disps[i][axis] += deltas[grid * side * side + i][axis];
      }
    }
  }
  return true;
}

bool bake_texture_displacement_to_active(Depsgraph *depsgraph,
                                         Object &object,
                                         const bool respect_mask,
                                         bool &r_used_mask)
{
  weight_hot_delta_cache_invalidate();
  r_used_mask = false;
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      BKE_modifiers_findby_type(&object, eModifierType_Multires));
  Mesh *mesh = BKE_mesh_from_object(&object);
  if (!mmd || !mesh || !mmd->sculpt_displacement_enabled ||
      !mmd->sculpt_displacement_texture || mmd->sculpt_displacement_strength == 0.0f)
  {
    return false;
  }

  MultiresSculptLayer *layer = active(*mmd);
  if (!layer || (layer->flag & MULTIRES_SCULPT_LAYER_VISIBLE) == 0 ||
      layer->disps == nullptr || layer->grids_num != mesh->corners_num ||
      std::abs(layer->strength) <= 1e-8f)
  {
    return false;
  }

  MultiresReshapeContext reshape_context{};
  if (!multires_reshape_context_create_from_object(
          &reshape_context, depsgraph, &object, mmd))
  {
    return false;
  }

  const Object *object_eval = DEG_get_evaluated(depsgraph, &object);
  const Object *map_eval = mmd->sculpt_displacement_map_object ?
      DEG_get_evaluated(depsgraph, mmd->sculpt_displacement_map_object) : nullptr;
  const float4x4 texture_matrix = multires_texture_mapping_matrix(*mmd, *object_eval, map_eval);
  const int side = subdiv::grid_size_from_level(layer->level);
  if (side < 2) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  const float inv = 1.0f / float(side - 1);
  const GridPaintMask *masks = static_cast<const GridPaintMask *>(
      CustomData_get_layer(&mesh->corner_data, CD_GRID_PAINT_MASK));
  const float inv_strength = 1.0f / layer->strength;

  for (int grid_index = 0; grid_index < layer->grids_num; grid_index++) {
    MDisps &grid = layer->disps[grid_index];

    /* Sparse Sculpt Layer grids are implicit zero until an operation writes
     * to them. Texture bake can affect every grid, so materialize this grid
     * on demand before storing the baked displacement. */
    if (grid.totdisp != side * side ||
        grid_ensure(*layer, grid_index) == nullptr)
    {
      multires_reshape_context_free(&reshape_context);
      return false;
    }

    for (int y = 0; y < side; y++) {
      for (int x = 0; x < side; x++) {
        const GridCoord grid_coord{grid_index, float(x) * inv, float(y) * inv};
        const PTexCoord ptex = multires_reshape_grid_coord_to_ptex(&reshape_context, &grid_coord);
        float influence = 1.0f;
        if (respect_mask && masks && masks[grid_index].data) {
          const GridPaintMask &mask_grid = masks[grid_index];
          const int mask_side = CCG_grid_size(mask_grid.level);
          const int mx = std::clamp(int(std::round(float(x) * float(mask_side - 1) * inv)),
                                    0,
                                    mask_side - 1);
          const int my = std::clamp(int(std::round(float(y) * float(mask_side - 1) * inv)),
                                    0,
                                    mask_side - 1);
          const float mask = std::clamp(mask_grid.data[my * mask_side + mx], 0.0f, 1.0f);
          if (mask > 0.0f) {
            r_used_mask = true;
          }
          influence = 1.0f - mask;
        }

        const float delta = multires_texture_displacement_sample(*reshape_context.base_mesh,
                                                                  *reshape_context.subdiv,
                                                                  *mmd,
                                                                  ptex.ptex_face_index,
                                                                  ptex.u,
                                                                  ptex.v,
                                                                  &texture_matrix) *
                            influence;
        grid.disps[y * side + x][2] += delta * inv_strength;
      }
    }
  }

  multires_reshape_context_free(&reshape_context);
  return true;
}

/* V4.5a sparse Sculpt Layer foundation.
 *
 * The MDisps descriptor array stays dense and cheap, while the high-resolution float[3] payload
 * can be materialized one CCG grid at a time. A null payload is semantically an all-zero layer
 * contribution for that grid. */
float (*grid_ensure(MultiresSculptLayer &layer, const int corner))[3]
{
  if (layer.disps == nullptr || corner < 0 || corner >= layer.grids_num ||
      layer.level <= 0 || layer.level > 13)
  {
    return nullptr;
  }

  const int side = subdiv::grid_size_from_level(layer.level);
  if (side < 2) {
    return nullptr;
  }
  const int area = side * side;

  MDisps &grid = layer.disps[corner];

  /* Future sparse layers initialize descriptor metadata up front. Be tolerant of a completely
   * empty descriptor as well, so old/new construction paths converge here safely. */
  if (grid.disps == nullptr && grid.totdisp == 0 && grid.level == 0) {
    grid.totdisp = area;
    grid.level = layer.level;
  }

  if (grid.level != layer.level || grid.totdisp != area) {
    return nullptr;
  }

  if (grid.disps == nullptr) {
    grid.disps = MEM_new_array_zeroed<float[3]>(area, "multires sparse sculpt layer grid");
    weight_hot_delta_cache_invalidate(&layer);
  }

  return grid.disps;
}
bool sample_tangent(const MultiresSculptLayer &layer,
                    const int corner,
                    const int /*grid_size*/,
                    const float grid_u,
                    const float grid_v,
                    float r_tangent_D[3])
{
  if ((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) == 0 || layer.disps == nullptr ||
      corner < 0 || corner >= layer.grids_num || (layer.level <= 0 || layer.level > 13) ||
      !std::isfinite(layer.strength) || !std::isfinite(grid_u) || !std::isfinite(grid_v) ||
      layer.disps[corner].disps == nullptr)
  {
    return false;
  }
  const MDisps &grid = layer.disps[corner];
  const int layer_grid_size = subdiv::grid_size_from_level(layer.level);
  if (grid.level != layer.level || grid.totdisp != layer_grid_size * layer_grid_size) {
    return false;
  }
  /* Sample in normalized grid coordinates. This deliberately allows a layer to be
   * stored at a different level than the currently evaluated sculpt level. */
  const float fx = std::clamp(grid_u * float(layer_grid_size - 1),
                              0.0f,
                              float(layer_grid_size - 1));
  const float fy = std::clamp(grid_v * float(layer_grid_size - 1),
                              0.0f,
                              float(layer_grid_size - 1));
  const int x0 = int(std::floor(fx));
  const int y0 = int(std::floor(fy));
  const int x1 = std::min(x0 + 1, layer_grid_size - 1);
  const int y1 = std::min(y0 + 1, layer_grid_size - 1);
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);
  const float *v00 = grid.disps[y0 * layer_grid_size + x0];
  const float *v10 = grid.disps[y0 * layer_grid_size + x1];
  const float *v01 = grid.disps[y1 * layer_grid_size + x0];
  const float *v11 = grid.disps[y1 * layer_grid_size + x1];
  float sampled[3];
  for (int axis = 0; axis < 3; axis++) {
    if (!std::isfinite(v00[axis]) || !std::isfinite(v10[axis]) ||
        !std::isfinite(v01[axis]) || !std::isfinite(v11[axis])) {
      return false;
    }
    const float a = std::lerp(v00[axis], v10[axis], tx);
    const float b = std::lerp(v01[axis], v11[axis], tx);
    sampled[axis] = std::lerp(a, b, ty) * layer.strength;
    if (!std::isfinite(sampled[axis])) {
      return false;
    }
  }
  std::copy_n(sampled, 3, r_tangent_D);
  return true;
}



/* SHAPEKEY_SUPPORT_CACHE_V2_1B
 *
 * Cache only the stable support region (coarse faces/grids). PBVH node indices
 * are intentionally rebuilt every update because the Sculpt PBVH may be rebuilt
 * or repartitioned while the object remains alive.
 */
struct ShapeKeySupportCache {
  const Object *object = nullptr;
  const Key *key = nullptr;
  const KeyBlock *key_block = nullptr;
  const KeyBlock *relative = nullptr;
  const void *key_data = nullptr;
  const void *relative_data = nullptr;
  const Mesh *mesh = nullptr;
  const int *corner_verts_data = nullptr;
  int verts_num = 0;
  int faces_num = 0;
  int corners_num = 0;
  int affected_vert_count = 0;
  Vector<int> dirty_faces;
  Vector<int> dirty_grids;

  /* SHAPEKEY_DUAL_PATH_V28
   * Cached mapping from changed Mesh vertices to OpenSubdiv coarse-control indices.
   * Runtime evaluator identity is intentionally not part of support-cache validity. */
  Vector<int> control_verts;
  Vector<int> evaluator_indices;
};

static Vector<ShapeKeySupportCache> &shape_key_support_caches()
{
  static Vector<ShapeKeySupportCache> caches;
  return caches;
}

void shape_key_fast_cache_invalidate()
{
  shape_key_support_caches().clear();
}

static ShapeKeySupportCache *shape_key_support_cache_find(const Object &object,
                                                          const KeyBlock &key_block)
{
  for (ShapeKeySupportCache &cache : shape_key_support_caches()) {
    if (cache.object == &object && cache.key_block == &key_block) {
      return &cache;
    }
  }
  return nullptr;
}

static bool shape_key_support_cache_valid(const ShapeKeySupportCache &cache,
                                          const Object &object,
                                          const Key &key,
                                          const KeyBlock &key_block,
                                          const KeyBlock &relative,
                                          const Mesh &mesh)
{
  return cache.object == &object && cache.key == &key && cache.key_block == &key_block &&
         cache.relative == &relative && cache.key_data == key_block.data &&
         cache.relative_data == relative.data && cache.mesh == &mesh &&
         cache.corner_verts_data == mesh.corner_verts().data() &&
         cache.verts_num == mesh.verts_num && cache.faces_num == mesh.faces_num &&
         cache.corners_num == mesh.corners_num;
}

static bool shape_key_local_trace_enabled()
{
  const char *value = BLI_getenv("BLENDER_SHAPEKEY_LOCAL_TRACE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool shape_key_fast_update(Object &object, Key &key, KeyBlock &key_block)
{
  const double total_start = BLI_time_now_seconds();

  if ((object.mode & OB_MODE_SCULPT) == 0 || object.type != OB_MESH ||
      object.runtime == nullptr || object.runtime->sculpt_session == nullptr ||
      key.type != KEY_RELATIVE || key.refkey == &key_block)
  {
    return false;
  }

  if (key_block.flag & KEYBLOCK_MUTE) {
    return true;
  }

  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG *ccg = ss.subdiv_ccg;
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (ccg == nullptr || ccg->subdiv == nullptr || pbvh == nullptr ||
      pbvh->type() != bke::pbvh::Type::Grids || ss.cache != nullptr ||
      ss.filter_cache != nullptr)
  {
    return false;
  }

  if (ccg->dirty.coords || ccg->dirty.hidden || ccg->dirty.masks) {
    return false;
  }

  Mesh *mesh = id_cast<Mesh *>(object.data);
  if (mesh == nullptr || mesh->verts_num <= 0 || mesh->faces_num <= 0 ||
      ccg->grids_num != mesh->corners_num)
  {
    return false;
  }

  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object.modifiers) {
    if ((md.mode & eModifierMode_Realtime) == 0) {
      continue;
    }
    if (md.type == eModifierType_Multires) {
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
      break;
    }
    return false;
  }
  if (mmd == nullptr || mmd->totlvl <= 0 || mmd->sculpt_shrinkwrap_enabled) {
    return false;
  }

  KeyBlock *relative = BKE_keyblock_find_by_index(&key, key_block.relative);
  if (relative == nullptr || key_block.data == nullptr || relative->data == nullptr ||
      key_block.totelem != mesh->verts_num || relative->totelem != mesh->verts_num)
  {
    return false;
  }

  const double support_start = BLI_time_now_seconds();
  ShapeKeySupportCache *support = shape_key_support_cache_find(object, key_block);
  bool cache_hit = support != nullptr &&
                   shape_key_support_cache_valid(
                       *support, object, key, key_block, *relative, *mesh);

  if (!cache_hit) {
    ShapeKeySupportCache rebuilt;
    rebuilt.object = &object;
    rebuilt.key = &key;
    rebuilt.key_block = &key_block;
    rebuilt.relative = relative;
    rebuilt.key_data = key_block.data;
    rebuilt.relative_data = relative->data;
    rebuilt.mesh = mesh;
    rebuilt.corner_verts_data = mesh->corner_verts().data();
    rebuilt.verts_num = mesh->verts_num;
    rebuilt.faces_num = mesh->faces_num;
    rebuilt.corners_num = mesh->corners_num;

    const Span<float3> key_data(static_cast<const float3 *>(key_block.data), key_block.totelem);
    const Span<float3> relative_data(static_cast<const float3 *>(relative->data), relative->totelem);

    BitVector<> affected_verts(mesh->verts_num, false);
    constexpr float support_epsilon_sq = 1.0e-16f;
    for (const int vert : IndexRange(mesh->verts_num)) {
      const float3 delta = key_data[vert] - relative_data[vert];
      if (math::length_squared(delta) > support_epsilon_sq) {
        affected_verts[vert].set();
        rebuilt.affected_vert_count++;
      }
    }

    if (rebuilt.affected_vert_count == 0) {
      if (support != nullptr) {
        *support = std::move(rebuilt);
      }
      else {
        shape_key_support_caches().append(std::move(rebuilt));
      }
      return true;
    }

    const OffsetIndices faces = mesh->faces();
    const Span<int> corner_verts = mesh->corner_verts();
    const GroupedSpan<int> vert_to_face = mesh->vert_to_face_map();

    /* OpenSubdiv coarse controls contain only vertices referenced by faces,
     * compacted in original Mesh vertex order. Cache the mapping once. */
    BitVector<> used_control_verts(mesh->verts_num, false);
    for (const int vert : corner_verts) {
      if (vert >= 0 && vert < mesh->verts_num) {
        used_control_verts[vert].set();
      }
    }
    int evaluator_index = 0;
    for (const int vert : IndexRange(mesh->verts_num)) {
      if (!used_control_verts[vert]) {
        continue;
      }
      if (affected_verts[vert]) {
        rebuilt.control_verts.append(vert);
        rebuilt.evaluator_indices.append(evaluator_index);
      }
      evaluator_index++;
    }

    /* SHAPEKEY_DIRTY_SUPPORT_V27
     *
     * A regular Catmull-Clark/B-spline patch influenced by a coarse vertex is
     * covered by the directly incident faces plus one vertex-adjacent face
     * ring. The old path expanded two complete rings, which is deliberately
     * conservative but often evaluates substantially more CCG patches.
     *
     * Keep the tighter one-ring support in regular quad regions, then restore
     * the second ring only around extraordinary/boundary/non-quad topology.
     * This preserves a conservative halo around poles while avoiding a global
     * second-ring expansion on ordinary surface regions.
     *
     * Debug override:
     *   BLENDER_SHAPEKEY_DIRTY_SUPPORT=1  -> force one ring
     *   BLENDER_SHAPEKEY_DIRTY_SUPPORT=2  -> force legacy two rings
     *   unset / anything else             -> topology-aware auto mode
     */
    BitVector<> direct_faces(mesh->faces_num, false);
    for (const int vert : IndexRange(mesh->verts_num)) {
      if (!affected_verts[vert]) {
        continue;
      }
      for (const int face : vert_to_face[vert]) {
        direct_faces[face].set();
      }
    }

    const auto expand_vertex_face_ring = [&](const BitVector<> &source) {
      BitVector<> expanded(mesh->faces_num, false);
      for (const int face : IndexRange(mesh->faces_num)) {
        if (!source[face]) {
          continue;
        }
        expanded[face].set();
        for (const int corner : faces[face]) {
          const int vert = corner_verts[corner];
          for (const int neighbor_face : vert_to_face[vert]) {
            expanded[neighbor_face].set();
          }
        }
      }
      return expanded;
    };

    const auto count_faces = [](const BitVector<> &bits) {
      int count = 0;
      for (const bool value : bits) {
        count += int(value);
      }
      return count;
    };

    BitVector<> ring1_faces = expand_vertex_face_ring(direct_faces);
    BitVector<> dirty_faces = ring1_faces;

    int support_mode = 0; /* 0 = topology-aware auto. */
    if (const char *support_env = BLI_getenv("BLENDER_SHAPEKEY_DIRTY_SUPPORT")) {
      if (support_env[0] == '1' && support_env[1] == '\0') {
        support_mode = 1;
      }
      else if (support_env[0] == '2' && support_env[1] == '\0') {
        support_mode = 2;
      }
    }

    int irregular_seed_faces = 0;
    if (support_mode == 2) {
      dirty_faces = expand_vertex_face_ring(ring1_faces);
    }
    else if (support_mode == 0) {
      BitVector<> irregular_faces(mesh->faces_num, false);

      for (const int face : IndexRange(mesh->faces_num)) {
        if (!ring1_faces[face]) {
          continue;
        }

        bool irregular = faces[face].size() != 4;
        if (!irregular) {
          for (const int corner : faces[face]) {
            const int vert = corner_verts[corner];

            /* On a closed regular quad surface a smooth interior vertex has
             * four incident coarse faces. Boundary vertices, poles and most
             * irregular neighborhoods therefore take the conservative path. */
            if (vert_to_face[vert].size() != 4) {
              irregular = true;
              break;
            }
          }
        }

        if (irregular) {
          irregular_faces[face].set();
          irregular_seed_faces++;
        }
      }

      if (irregular_seed_faces > 0) {
        const BitVector<> irregular_halo = expand_vertex_face_ring(irregular_faces);
        for (const int face : IndexRange(mesh->faces_num)) {
          if (irregular_halo[face]) {
            dirty_faces[face].set();
          }
        }
      }
    }

    if (shape_key_local_trace_enabled()) {
      const int direct_count = count_faces(direct_faces);
      const int ring1_count = count_faces(ring1_faces);
      const int final_count = count_faces(dirty_faces);

      /* Compute the former region only for diagnostics; never on cache hits. */
      const BitVector<> legacy_faces = expand_vertex_face_ring(ring1_faces);
      const int legacy_count = count_faces(legacy_faces);
      const char *mode_name = support_mode == 1 ? "RING1" :
                              support_mode == 2 ? "LEGACY2" : "AUTO";

      std::printf(
          "[SHAPEKEY_SUPPORT] mode=%s direct=%d ring1=%d irregular=%d "
          "final=%d legacy=%d saved=%d\n",
          mode_name,
          direct_count,
          ring1_count,
          irregular_seed_faces,
          final_count,
          legacy_count,
          std::max(0, legacy_count - final_count));
    }

    rebuilt.dirty_faces.reserve(mesh->faces_num / 4);
    for (const int face : IndexRange(mesh->faces_num)) {
      if (dirty_faces[face]) {
        rebuilt.dirty_faces.append(face);
      }
    }

    if (rebuilt.dirty_faces.is_empty()) {
      return false;
    }

    /* SHAPEKEY_BROAD_FAST_V271
     *
     * The previous V2 cache rejected any support region >= 50% of the coarse
     * mesh. That cutoff was intentionally conservative, but it also forces a
     * full depsgraph rebuild for broad facial/body Shape Keys even though the
     * existing CCG can still be refreshed safely in-place.
     *
     * Default to allowing the complete coarse-face range through the in-place
     * path. A diagnostic cap can still be requested with:
     *
     *   BLENDER_SHAPEKEY_LOCAL_MAX_PERCENT=50
     *   BLENDER_SHAPEKEY_LOCAL_MAX_PERCENT=75
     *   BLENDER_SHAPEKEY_LOCAL_MAX_PERCENT=90
     *   BLENDER_SHAPEKEY_LOCAL_MAX_PERCENT=95
     *   BLENDER_SHAPEKEY_LOCAL_MAX_PERCENT=100
     *
     * This is a performance policy only; all of the existing correctness
     * guards remain unchanged.
     */
    int max_local_percent = 100;
    if (const char *value = BLI_getenv("BLENDER_SHAPEKEY_LOCAL_MAX_PERCENT")) {
      int parsed = 0;
      bool valid = value[0] != '\0';
      for (int i = 0; value[i] != '\0'; i++) {
        if (value[i] < '0' || value[i] > '9') {
          valid = false;
          break;
        }
        parsed = parsed * 10 + int(value[i] - '0');
        if (parsed > 1000) {
          valid = false;
          break;
        }
      }
      if (valid) {
        max_local_percent = std::clamp(parsed, 1, 100);
      }
    }

    const int64_t support_percent_num =
        int64_t(rebuilt.dirty_faces.size()) * int64_t(100);
    const int64_t support_percent_den = int64_t(mesh->faces_num);
    const bool broad_fallback =
        support_percent_num > support_percent_den * int64_t(max_local_percent);

    if (shape_key_local_trace_enabled()) {
      const double ratio = mesh->faces_num > 0 ?
          (100.0 * double(rebuilt.dirty_faces.size()) / double(mesh->faces_num)) :
          100.0;
      std::printf(
          "[SHAPEKEY_SUPPORT_LIMIT] faces=%d/%d ratio=%.2f%% max=%d%% action=%s\n",
          int(rebuilt.dirty_faces.size()),
          mesh->faces_num,
          ratio,
          max_local_percent,
          broad_fallback ? "FALLBACK" : "LOCAL");
    }

    if (broad_fallback) {
      return false;
    }

    for (const int face : rebuilt.dirty_faces) {
      for (const int grid : faces[face]) {
        rebuilt.dirty_grids.append(grid);
      }
    }

    if (support != nullptr) {
      *support = std::move(rebuilt);
    }
    else {
      shape_key_support_caches().append(std::move(rebuilt));
      support = &shape_key_support_caches()[shape_key_support_caches().size() - 1];
    }
  }

  if (support == nullptr || support->affected_vert_count == 0) {
    return support != nullptr;
  }

  const double support_ms = (BLI_time_now_seconds() - support_start) * 1000.0;

  const double cage_start = BLI_time_now_seconds();
  Array<float3> cage_positions(mesh->verts_num);
  int evaluated_verts = 0;
  if (BKE_key_evaluate_object_ex(&object,
                                 &evaluated_verts,
                                 reinterpret_cast<float *>(cage_positions.data()),
                                 sizeof(float3) * size_t(cage_positions.size()),
                                 std::nullopt,
                                 nullptr) == nullptr ||
      evaluated_verts != mesh->verts_num)
  {
    return false;
  }

  Mesh *cage = BKE_mesh_copy_for_eval(*mesh);
  if (cage == nullptr) {
    return false;
  }
  BLI_SCOPED_DEFER([&]() { BKE_id_free(nullptr, cage); });

  cage->vert_positions_for_write().copy_from(cage_positions);
  if (!base_deltas_apply(*mmd, cage->vert_positions_for_write())) {
    return false;
  }
  cage->tag_positions_changed();
  const double cage_ms = (BLI_time_now_seconds() - cage_start) * 1000.0;

  bke::subdiv::Subdiv *subdiv = ccg->subdiv;

  const double attach_start = BLI_time_now_seconds();
  bke::subdiv::displacement_attach_from_multires(
      subdiv, mesh, mmd, multires_texture_mapping_matrix(*mmd, object));
  const double attach_ms = (BLI_time_now_seconds() - attach_start) * 1000.0;

  /* SHAPEKEY_V282_CURRENT_EVALUATOR
   * V2.8.2 local path: update only changed coarse controls on cache hits.
   *
   * SculptSession's live CCG may replace its Subdiv runtime between slider
   * updates. Pointer identity is therefore not a valid continuity test.
   * The current ccg->subdiv already owns an initialized evaluator for the
   * currently visible CCG state, so a cache hit can update the changed coarse
   * controls on that current evaluator and refine it in-place.
   *
   * Cache MISS still uses eval_begin_from_mesh() to establish a full baseline. */
  bool incremental_control_enabled = true;
  if (const char *value = BLI_getenv("BLENDER_SHAPEKEY_INCREMENTAL_CONTROL")) {
    incremental_control_enabled = value[0] != '\0' && value[0] != '0';
  }

  int incremental_max_percent = 20;
  if (const char *value = BLI_getenv("BLENDER_SHAPEKEY_INCREMENTAL_CONTROL_MAX_PERCENT")) {
    int parsed = 0;
    bool valid = value[0] != '\0';
    for (int i = 0; value[i] != '\0'; i++) {
      if (value[i] < '0' || value[i] > '9') {
        valid = false;
        break;
      }
      parsed = parsed * 10 + int(value[i] - '0');
      if (parsed > 1000) {
        valid = false;
        break;
      }
    }
    if (valid) {
      incremental_max_percent = std::clamp(parsed, 1, 100);
    }
  }

  const int64_t support_percent_num =
      int64_t(support->dirty_faces.size()) * int64_t(100);
  const int64_t support_percent_den = int64_t(mesh->faces_num);
  const bool support_is_local =
      support_percent_num <= support_percent_den * int64_t(incremental_max_percent);

  double control_upload_ms = 0.0;
  double control_refine_ms = 0.0;
  double full_evaluator_ms = 0.0;
  int control_runs = 0;
  bool used_incremental_control = false;

#ifdef WITH_OPENSUBDIV
  /* SHAPEKEY_V282_STABLE
   * Stable local-control path. Runtime Subdiv pointer identity is deliberately
   * not a guard: Sculpt CCG may replace the Subdiv while the current evaluator
   * remains fully initialized and valid for incremental coarse-control updates. */
  const bool incremental_controls_match =
      support->control_verts.size() == support->evaluator_indices.size();
  const bool incremental_controls_nonempty = !support->control_verts.is_empty();
  const bool incremental_has_evaluator = subdiv->evaluator != nullptr;
  const bool incremental_has_eval_output =
      incremental_has_evaluator && subdiv->evaluator->eval_output != nullptr;

  if (incremental_control_enabled && cache_hit && support_is_local &&
      incremental_controls_match && incremental_controls_nonempty &&
      incremental_has_evaluator && incremental_has_eval_output)
  {
    const Span<float3> cage_positions_current = cage->vert_positions();
    Array<float3> packed_positions(support->control_verts.size());
    for (const int i : packed_positions.index_range()) {
      const int vert = support->control_verts[i];
      if (vert < 0 || vert >= cage_positions_current.size()) {
        return false;
      }
      packed_positions[i] = cage_positions_current[vert];
    }

    const double upload_start = BLI_time_now_seconds();
    int run_start = 0;
    while (run_start < support->evaluator_indices.size()) {
      int run_end = run_start + 1;
      while (run_end < support->evaluator_indices.size() &&
             support->evaluator_indices[run_end] ==
                 support->evaluator_indices[run_end - 1] + 1)
      {
        run_end++;
      }

      subdiv->evaluator->eval_output->setCoarsePositions(
          reinterpret_cast<const float *>(packed_positions.data() + run_start),
          support->evaluator_indices[run_start],
          run_end - run_start);
      control_runs++;
      run_start = run_end;
    }
    control_upload_ms = (BLI_time_now_seconds() - upload_start) * 1000.0;

    const double refine_start = BLI_time_now_seconds();
    subdiv->evaluator->eval_output->refine();
    control_refine_ms = (BLI_time_now_seconds() - refine_start) * 1000.0;
    used_incremental_control = true;
  }
#endif

  if (!used_incremental_control) {
    const double evaluator_start = BLI_time_now_seconds();
    if (!bke::subdiv::eval_begin_from_mesh(
            subdiv, cage, bke::subdiv::SUBDIV_EVALUATOR_TYPE_CPU))
    {
      return false;
    }
    full_evaluator_ms = (BLI_time_now_seconds() - evaluator_start) * 1000.0;
  }

  const double evaluator_ms = used_incremental_control ?
                                  control_upload_ms + control_refine_ms :
                                  full_evaluator_ms;
  const double subdiv_ms = attach_ms + evaluator_ms;

  if (shape_key_local_trace_enabled()) {
    std::printf(
        "[SHAPEKEY_OSD] mode=%s controls=%d runs=%d max_local=%d%% "
        "upload=%.3fms refine=%.3fms full=%.3fms total=%.3fms\n",
        used_incremental_control ? "INCREMENTAL" : "FULL",
        int(support->control_verts.size()),
        control_runs,
        incremental_max_percent,
        control_upload_ms,
        control_refine_ms,
        full_evaluator_ms,
        evaluator_ms);
  }

  const double ccg_start = BLI_time_now_seconds();
  IndexMaskMemory face_memory;
  const IndexMask face_mask = IndexMask::from_indices(support->dirty_faces.as_span(), face_memory);

  SubdivCCGMaskEvaluator mask_evaluator{};
  const bool has_mask = !ccg->masks.is_empty() &&
      BKE_subdiv_ccg_mask_init_from_paint(&mask_evaluator, mesh);
  const bool eval_ok = BKE_subdiv_ccg_evaluate_faces_existing(
      *ccg, *subdiv, face_mask, has_mask ? &mask_evaluator : nullptr);
  if (has_mask) {
    mask_evaluator.free(&mask_evaluator);
  }
  if (!eval_ok) {
    return false;
  }
  const double ccg_ms = (BLI_time_now_seconds() - ccg_start) * 1000.0;

  const double pbvh_start = BLI_time_now_seconds();

  BitVector<> dirty_grids(ccg->grids_num, false);
  for (const int grid : support->dirty_grids) {
    if (grid >= 0 && grid < ccg->grids_num) {
      dirty_grids[grid].set();
    }
  }

  const Span<bke::pbvh::GridsNode> nodes = pbvh->nodes<bke::pbvh::GridsNode>();
  IndexMaskMemory node_memory;
  const IndexMask changed_nodes = IndexMask::from_predicate(
      nodes.index_range(),
      node_memory,
      [&](const int node_i) {
        for (const int grid : nodes[node_i].grids()) {
          if (grid >= 0 && grid < dirty_grids.size() && dirty_grids[grid]) {
            return true;
          }
        }
        return false;
      },
      exec_mode::grain_size(1));

  if (changed_nodes.is_empty()) {
    return false;
  }

  pbvh->tag_positions_changed(changed_nodes);
  pbvh->update_bounds_grids(ccg->positions, ccg->grid_area);
  pbvh->flush_bounds_to_parents();

  pbvh->clear_changes();
  ccg->dirty.coords = false;

  const double pbvh_ms = (BLI_time_now_seconds() - pbvh_start) * 1000.0;
  const double total_ms = (BLI_time_now_seconds() - total_start) * 1000.0;

  if (shape_key_local_trace_enabled()) {
    std::printf(
        "[SHAPEKEY_LOCAL] key='%s' cache=%s verts=%d/%d faces=%d/%d nodes=%d/%d "
        "support=%.3fms cage=%.3fms attach=%.3fms evaluator=%.3fms "
        "subdiv=%.3fms ccg=%.3fms pbvh=%.3fms total=%.3fms\n",
        key_block.name,
        cache_hit ? "HIT" : "MISS",
        support->affected_vert_count,
        mesh->verts_num,
        int(support->dirty_faces.size()),
        mesh->faces_num,
        int(changed_nodes.size()),
        int(nodes.size()),
        support_ms,
        cage_ms,
        attach_ms,
        evaluator_ms,
        subdiv_ms,
        ccg_ms,
        pbvh_ms,
        total_ms);
  }

  return true;
}

}  // namespace blender::bke::multires_layers


