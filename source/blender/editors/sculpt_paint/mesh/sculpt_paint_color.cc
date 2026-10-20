/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "DNA_brush_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#include "BLI_color.hh"
#include "BLI_time.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_hash.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_multires_polypaint_prepared.hh"
#include <unordered_set>
#include <vector>
#include "BKE_subdiv.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "IMB_colormanagement.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_intern.hh"
#include "sculpt_smooth.hh"

#include "IMB_imbuf.hh"

#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "sculpt_polypaint_blur_v01.hh"

namespace blender::ed::sculpt_paint::color {

static void calc_local_positions(const float4x4 &mat,
                                 const Span<int> verts,
                                 const Span<float3> positions,
                                 const MutableSpan<float3> local_positions)
{
  PRF_scope(ProfileCategory::Editor);
  for (const int i : verts.index_range()) {
    local_positions[i] = math::transform_point(mat, positions[verts[i]]);
  }
}

template<typename Func> inline void to_static_color_type(const CPPType &type, const Func &func)
{
  if (type.is<ColorGeometry4f>()) {
    func(MPropCol());
  }
  else if (type.is<ColorGeometry4b>()) {
    func(MLoopCol());
  }
}

template<typename T> float4 to_float(const T &src);

template<> float4 to_float(const MLoopCol &src)
{
  float4 dst;
  rgba_uchar_to_float(dst, reinterpret_cast<const uchar *>(&src));
  IMB_colormanagement_srgb_to_scene_linear_v3(dst, dst);
  return dst;
}
template<> float4 to_float(const MPropCol &src)
{
  return src.color;
}

template<typename T> void from_float(const float4 &src, T &dst);

template<> void from_float(const float4 &src, MLoopCol &dst)
{
  float4 temp;
  IMB_colormanagement_scene_linear_to_srgb_v3(temp, src);
  temp[3] = src[3];
  rgba_float_to_uchar(reinterpret_cast<uchar *>(&dst), temp);
}
template<> void from_float(const float4 &src, MPropCol &dst)
{
  copy_v4_v4(dst.color, src);
}

template<typename T>
static float4 color_vert_get(const OffsetIndices<int> faces,
                             const Span<int> corner_verts,
                             const GroupedSpan<int> vert_to_face_map,
                             const GSpan color_attribute,
                             const bke::AttrDomain color_domain,
                             const int vert)
{
  const T *colors_typed = static_cast<const T *>(color_attribute.data());
  if (color_domain == bke::AttrDomain::Corner) {
    float4 r_color(0.0f);
    for (const int face : vert_to_face_map[vert]) {
      const int corner = bke::mesh::face_find_corner_from_vert(faces[face], corner_verts, vert);
      r_color += to_float(colors_typed[corner]);
    }
    return r_color / float(vert_to_face_map[vert].size());
  }
  return to_float(colors_typed[vert]);
}

template<typename T>
static void color_vert_set(const OffsetIndices<int> faces,
                           const Span<int> corner_verts,
                           const GroupedSpan<int> vert_to_face_map,
                           const GMutableSpan color_attribute,
                           const bke::AttrDomain color_domain,
                           const int vert,
                           const float4 &color)
{
  if (color_domain == bke::AttrDomain::Corner) {
    for (const int i_face : vert_to_face_map[vert]) {
      const IndexRange face = faces[i_face];
      MutableSpan<T> colors{static_cast<T *>(color_attribute.data()) + face.start(), face.size()};
      Span<int> face_verts = corner_verts.slice(face);

      for (const int i : IndexRange(face.size())) {
        if (face_verts[i] == vert) {
          from_float(color, colors[i]);
        }
      }
    }
  }
  else {
    from_float(color, static_cast<T *>(color_attribute.data())[vert]);
  }
}

float4 color_vert_get(const OffsetIndices<int> faces,
                      const Span<int> corner_verts,
                      const GroupedSpan<int> vert_to_face_map,
                      const GSpan color_attribute,
                      const bke::AttrDomain color_domain,
                      const int vert)
{
  float4 color;
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    color = color_vert_get<T>(
        faces, corner_verts, vert_to_face_map, color_attribute, color_domain, vert);
  });
  return color;
}

void color_vert_set(const OffsetIndices<int> faces,
                    const Span<int> corner_verts,
                    const GroupedSpan<int> vert_to_face_map,
                    const bke::AttrDomain color_domain,
                    const int vert,
                    const float4 &color,
                    const GMutableSpan color_attribute)
{
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    color_vert_set<T>(
        faces, corner_verts, vert_to_face_map, color_attribute, color_domain, vert, color);
  });
}

void swap_gathered_colors(const Span<int> indices,
                          GMutableSpan color_attribute,
                          MutableSpan<float4> r_colors)
{
  PRF_scope(ProfileCategory::Editor);
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    T *colors_typed = static_cast<T *>(color_attribute.data());
    for (const int i : indices.index_range()) {
      T temp = colors_typed[indices[i]];
      from_float(r_colors[i], colors_typed[indices[i]]);
      r_colors[i] = to_float(temp);
    }
  });
}

void gather_colors(const GSpan color_attribute,
                   const Span<int> indices,
                   MutableSpan<float4> r_colors)
{
  to_static_color_type(color_attribute.type(), [&](auto dummy) {
    using T = decltype(dummy);
    const T *colors_typed = static_cast<const T *>(color_attribute.data());
    for (const int i : indices.index_range()) {
      r_colors[i] = to_float(colors_typed[indices[i]]);
    }
  });
}

void gather_colors_vert(const OffsetIndices<int> faces,
                        const Span<int> corner_verts,
                        const GroupedSpan<int> vert_to_face_map,
                        const GSpan color_attribute,
                        const bke::AttrDomain color_domain,
                        const Span<int> verts,
                        const MutableSpan<float4> r_colors)
{
  PRF_scope(ProfileCategory::Editor);
  if (color_domain == bke::AttrDomain::Point) {
    gather_colors(color_attribute, verts, r_colors);
  }
  else {
    to_static_color_type(color_attribute.type(), [&](auto dummy) {
      using T = decltype(dummy);
      for (const int i : verts.index_range()) {
        r_colors[i] = color_vert_get<T>(
            faces, corner_verts, vert_to_face_map, color_attribute, color_domain, verts[i]);
      }
    });
  }
}

bke::GAttributeReader active_color_attribute(const Mesh &mesh)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  const StringRef name = mesh.active_color_attribute;
  const bke::GAttributeReader colors = attributes.lookup(name);
  if (!colors) {
    return {};
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(colors.varray.type());
  if (!bke::mesh::is_color_attribute({colors.domain, data_type})) {
    return {};
  }
  return colors;
}

bke::GSpanAttributeWriter active_color_attribute_for_write(Mesh &mesh)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const StringRef name = mesh.active_color_attribute;
  bke::GSpanAttributeWriter colors = attributes.lookup_for_write_span(name);
  if (!colors) {
    return {};
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(colors.span.type());
  if (!bke::mesh::is_color_attribute({colors.domain, data_type})) {
    colors.finish();
    return {};
  }
  return colors;
}

struct ColorPaintLocalData {
  Vector<float> factors;
  Vector<float> auto_mask;
  Vector<float3> positions;
  Vector<float> distances;
  Vector<float4> colors;
  Vector<float4> new_colors;
  Vector<float4> mix_colors;
  Vector<int> neighbor_offsets;
  Vector<int> neighbor_data;
};

static void do_color_smooth_task(const Depsgraph &depsgraph,
                                 const Object &object,
                                 const Span<float3> vert_positions,
                                 const Span<float3> vert_normals,
                                 const OffsetIndices<int> faces,
                                 const Span<int> corner_verts,
                                 const GroupedSpan<int> vert_to_face_map,
                                 const MeshAttributeData &attribute_data,
                                 const Brush &brush,
                                 const bke::pbvh::MeshNode &node,
                                 ColorPaintLocalData &tls,
                                 bke::GSpanAttributeWriter &color_attribute)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
  scale_factors(factors, cache.bstrength);

  tls.colors.resize(verts.size());
  MutableSpan<float4> colors = tls.colors;
  for (const int i : verts.index_range()) {
    colors[i] = color_vert_get(faces,
                               corner_verts,
                               vert_to_face_map,
                               color_attribute.span,
                               color_attribute.domain,
                               verts[i]);
  }

  const GroupedSpan<int> neighbors = calc_vert_neighbors(faces,
                                                         corner_verts,
                                                         vert_to_face_map,
                                                         attribute_data.hide_poly,
                                                         verts,
                                                         tls.neighbor_offsets,
                                                         tls.neighbor_data);

  tls.new_colors.resize(verts.size());
  MutableSpan<float4> new_colors = tls.new_colors;
  smooth::neighbor_color_average(faces,
                                 corner_verts,
                                 vert_to_face_map,
                                 color_attribute.span,
                                 color_attribute.domain,
                                 neighbors,
                                 new_colors);

  for (const int i : colors.index_range()) {
    blend_color_interpolate_float(new_colors[i], colors[i], new_colors[i], factors[i]);
  }

  for (const int i : verts.index_range()) {
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   verts[i],
                   new_colors[i],
                   color_attribute.span);
  }
}

static void do_paint_brush_task(const Depsgraph &depsgraph,
                                Object &object,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const GroupedSpan<int> vert_to_face_map,
                                const MeshAttributeData &attribute_data,
                                const Paint &paint,
                                const Brush &brush,
                                const float4x4 &mat,
                                const float4 wet_mix_sampled_color,
                                bke::pbvh::MeshNode &node,
                                ColorPaintLocalData &tls,
                                const MutableSpan<float4> mix_colors,
                                bke::GSpanAttributeWriter &color_attribute)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  const float bstrength = fabsf(ss.cache->bstrength);
  const float alpha = BKE_brush_alpha_get(&paint, &brush);

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  float radius;

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  if (brush.tip_roundness < 1.0f) {
    tls.positions.resize(verts.size());
    calc_local_positions(mat, verts, vert_positions, tls.positions);
    calc_brush_cube_distances<float3>(brush, tls.positions, distances);
    radius = 1.0f;
  }
  else {
    calc_brush_distances(
        ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
    radius = cache.radius;
  }
  filter_distances_with_radius(radius, distances, factors);
  apply_hardness_to_distances(radius, cache.hardness, distances);
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               radius,
                               factors);

  MutableSpan<float> auto_mask;
  if (cache.automasking) {
    tls.auto_mask.resize(verts.size());
    auto_mask = tls.auto_mask;
    auto_mask.fill(1.0f);
    auto_mask::calc_vert_factors(depsgraph, object, *cache.automasking, node, verts, auto_mask);
    scale_factors(factors, auto_mask);
  }

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
  scale_factors(factors, bstrength);

  const float density = ss.cache->paint_brush.density;
  if (density < 1.0f) {
    BLI_assert(ss.cache->paint_brush.density_seed);
    const float seed = ss.cache->paint_brush.density_seed.value_or(0.0f);
    for (const int i : verts.index_range()) {
      const float hash_noise = BLI_hash_int_01(seed * 1000 * verts[i]);
      if (hash_noise > density) {
        const float noise = density * hash_noise;
        factors[i] *= noise;
      }
    }
  }

  float3 brush_color_rgb = ss.cache->toggle_settings.invert ?
                               BKE_brush_secondary_color_get(&paint, &brush) :
                               BKE_brush_color_get(&paint, &brush);

  const std::optional<BrushColorJitterSettings> color_jitter_settings =
      BKE_brush_color_jitter_get_settings(&paint, &brush);
  if (color_jitter_settings) {
    brush_color_rgb = BKE_paint_randomize_color(*color_jitter_settings,
                                                *ss.cache->initial_hsv_jitter,
                                                ss.cache->stroke_distance,
                                                ss.cache->pressure,
                                                brush_color_rgb);
  }

  float4 brush_color(brush_color_rgb, 1.0f);

  const Span<float4> orig_colors = orig_color_data_get_mesh(object, node);

  MutableSpan<float4> color_buffer = gather_data_mesh(mix_colors.as_span(), verts, tls.mix_colors);

  if (brush.flag & BRUSH_USE_GRADIENT) {
    switch (brush.gradient_stroke_mode) {
      case BRUSH_GRADIENT_PRESSURE:
        BKE_colorband_evaluate(brush.gradient, ss.cache->pressure, brush_color);
        break;
      case BRUSH_GRADIENT_SPACING_REPEAT: {
        float coord = fmod(ss.cache->stroke_distance / brush.gradient_spacing, 1.0);
        BKE_colorband_evaluate(brush.gradient, coord, brush_color);
        break;
      }
      case BRUSH_GRADIENT_SPACING_CLAMP: {
        BKE_colorband_evaluate(
            brush.gradient, ss.cache->stroke_distance / brush.gradient_spacing, brush_color);
        break;
      }
    }
  }

  tls.new_colors.resize(verts.size());
  MutableSpan<float4> new_colors = tls.new_colors;
  for (const int i : verts.index_range()) {
    new_colors[i] = color_vert_get(faces,
                                   corner_verts,
                                   vert_to_face_map,
                                   color_attribute.span,
                                   color_attribute.domain,
                                   verts[i]);
  }

  for (const int i : verts.index_range()) {
    /* Brush paint color, brush test falloff and flow. */
    float4 paint_color = brush_color * factors[i] * ss.cache->paint_brush.flow;
    float4 wet_mix_color = wet_mix_sampled_color * factors[i] * ss.cache->paint_brush.flow;

    /* Interpolate with the wet_mix color for wet paint mixing. */
    blend_color_interpolate_float(
        paint_color, paint_color, wet_mix_color, ss.cache->paint_brush.wet_mix);
    blend_color_mix_float(color_buffer[i], color_buffer[i], paint_color);

    /* Final mix over the original color using brush alpha. We apply auto-making again
     * at this point to avoid washing out non-binary masking modes like cavity masking. */
    float automasking = auto_mask.is_empty() ? 1.0f : auto_mask[i];
    const float4 buffer_color = float4(color_buffer[i]) * alpha * automasking;

    IMB_blend_color_float(new_colors[i], orig_colors[i], buffer_color, IMB_BlendMode(brush.blend));
    new_colors[i] = math::clamp(new_colors[i], 0.0f, 1.0f);
  }

  scatter_data_mesh(color_buffer.as_span(), verts, mix_colors);

  for (const int i : verts.index_range()) {
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   verts[i],
                   new_colors[i],
                   color_attribute.span);
  }
}

struct SampleWetPaintData {
  int tot_samples;
  float4 color;
};

static void do_sample_wet_paint_task(const Depsgraph &depsgraph,
                                     const Object &object,
                                     const Span<float3> vert_positions,
                                     const Span<float3> vert_normals,
                                     const OffsetIndices<int> faces,
                                     const Span<int> corner_verts,
                                     const GroupedSpan<int> vert_to_face_map,
                                     const MeshAttributeData &attribute_data,
                                     const GSpan color_attribute,
                                     const bke::AttrDomain color_domain,
                                     const Brush &brush,
                                     const bke::pbvh::MeshNode &node,
                                     ColorPaintLocalData &tls,
                                     SampleWetPaintData &swptd)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const float radius = cache.radius * brush.wet_paint_radius_factor;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }
  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(radius, distances, factors);

  for (const int i : verts.index_range()) {
    if (factors[i] > 0.0f) {
      swptd.color +=
          color_vert_get(
              faces, corner_verts, vert_to_face_map, color_attribute, color_domain, verts[i]) *
          factors[i];
      swptd.tot_samples++;
    }
  }
}

/* POLYPAINT_V02B_CPU_GRIDS: deliberately serial until shared CCG edges have a
 * dedicated synchronization pass. The existing mesh-paint path stays unchanged.
 * Only the basic Paint operation is supported here; Smear/Blur are separate. */
static void do_paint_brush_grids_cpu_prepared_v02(const Depsgraph &depsgraph,
                                     const Sculpt &sd,
                                     Object &object,
                                     const IndexMask &node_mask)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_POLYPAINT_CPU");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || !ss.multires_modifier) {
    return;
  }
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double paint_start = trace ? BLI_time_now_seconds() : 0.0;
  MultiresModifierData &mmd = *ss.multires_modifier;
  SubdivCCG &ccg = *ss.subdiv_ccg;
  Mesh &base_mesh = *id_cast<Mesh *>(object.data);
  if (mmd.sculptlvl < 1 || mmd.sculptlvl > mmd.totlvl ||
      ccg.grid_size != bke::subdiv::grid_size_from_level(mmd.sculptlvl) ||
      ccg.grids_num != base_mesh.corners_num ||
      !active_color_attribute(base_mesh))
  {
    return;
  }
  const char *name = base_mesh.active_color_attribute;
  if (!name || name[0] == '\0') {
    return;
  }
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float3 primary = ss.cache->toggle_settings.invert ?
                             BKE_brush_secondary_color_get(&sd.paint, &brush) :
                             BKE_brush_color_get(&sd.paint, &brush);
  const float strength = std::clamp(
      std::abs(ss.cache->bstrength) * BKE_brush_alpha_get(&sd.paint, &brush) *
          ss.cache->paint_brush.flow,
      0.0f,
      1.0f);
  if (strength == 0.0f) {
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  bool changed = false;
  int64_t touched_samples = 0;
  const char *prepared_env = std::getenv("BLENDER_SCULPT_POLYPAINT_PREPARED_V2");
  const bool prepared_enabled = prepared_env && prepared_env[0] == '1' && prepared_env[1] == '\0';
  bke::multires_polypaint_cpu::PreparedPaintPlan prepared_plan;
  int64_t prepared_samples = 0, prepared_grids = 0;
  double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
  const double setup_end = trace ? BLI_time_now_seconds() : 0.0;

  /* The grid sampler and delta writer share the same master storage. Serial
   * execution prevents overlapping coarse-support writes across PBVH nodes. */
  node_mask.foreach_index([&](const int node_index) {
    const Span<int> grids = nodes[node_index].grids();
    double stage_start = trace ? BLI_time_now_seconds() : 0.0;
    const Span<float3> co = gather_grids_positions(ccg, grids, positions);
    if (trace) {
      const double now = BLI_time_now_seconds();
      gather_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    calc_factors_common_grids(
        depsgraph, brush, object, co, nodes[node_index], factors, distances);
    if (trace) {
      const double now = BLI_time_now_seconds();
      factors_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    int prepared_grid = -1;
    float (*prepared_master)[4] = nullptr;
    for (const int i : co.index_range()) {
      const float influence = std::clamp(factors[i] * strength, 0.0f, 1.0f);
      if (influence <= 0.0f) {
        continue;
      }
      const int local_grid = i / ccg.grid_area;
      const int pixel = i % ccg.grid_area;
      const int grid_index = grids[local_grid];
      const int x = pixel % ccg.grid_size;
      const int y = pixel / ccg.grid_size;
      if (prepared_grid != grid_index) {
        prepared_grid = grid_index;
        prepared_master = nullptr;
      }
      if (prepared_master) {
        const float brush_rgb[3] = {primary[0], primary[1], primary[2]};
        const bool wrote = prepared_plan.blend(prepared_master, x, y, brush_rgb, influence);
        changed |= wrote;
        touched_samples += int(wrote);
        prepared_samples += int(wrote);
        continue;
      }
      const float u = float(x) / float(ccg.grid_size - 1);
      const float v = float(y) / float(ccg.grid_size - 1);
      float before[4];
      if (!bke::multires_layers::polypaint_sample_with_base(
              mmd, base_mesh, name, grid_index, u, v, before))
      {
        continue;
      }
      float target[4];
      for (int channel = 0; channel < 3; channel++) {
        target[channel] = before[channel] * (1.0f - influence) +
                          primary[channel] * influence;
      }
      target[3] = before[3];
      const bool wrote = bke::multires_layers::polypaint_write_coarse_sample(
          mmd, base_mesh, name, grid_index, mmd.sculptlvl, x, y, target);
      if (wrote && prepared_enabled && bke::multires_layers::polypaint_grid(mmd, grid_index)) {
        const double start = trace ? BLI_time_now_seconds() : 0.0;
        const int master_side = bke::subdiv::grid_size_from_level(mmd.sculpt_color_level);
        if (prepared_plan.prepare(master_side, ccg.grid_size)) {
          prepared_master = mmd.sculpt_color_grids[grid_index].colors;
          prepared_grids++;
        }
        if (trace) prepare_ms += (BLI_time_now_seconds() - start) * 1000.0;
      }
      changed |= wrote;
      touched_samples += int(wrote);
    }
    if (trace) write_ms += (BLI_time_now_seconds() - stage_start) * 1000.0;
  });
  const double notify_start = trace ? BLI_time_now_seconds() : 0.0;
  if (changed) {
    pbvh.tag_attribute_changed(node_mask, name);
  }
  const double paint_end = trace ? BLI_time_now_seconds() : 0.0;
  if (trace) {
    std::printf("[POLYPAINT_CPU] paint_level=%d master_level=%d nodes=%lld "
                "touched=%lld changed=%d total=%.3fms\n",
                mmd.sculptlvl, mmd.sculpt_color_level,
                static_cast<long long>(node_mask.size()),
                static_cast<long long>(touched_samples), int(changed),
                (BLI_time_now_seconds() - paint_start) * 1000.0);
    std::printf("[POLYPAINT_PREPARED_V2] enabled=%d reused_samples=%lld prepared_grids=%lld "
                "setup=%.3fms gather=%.3fms factors=%.3fms write=%.3fms "
                "prepare=%.3fms notify=%.3fms\n",
                int(prepared_enabled), static_cast<long long>(prepared_samples),
                static_cast<long long>(prepared_grids), (setup_end - paint_start) * 1000.0,
                gather_ms, factors_ms, write_ms, prepare_ms,
                (paint_end - notify_start) * 1000.0);
  }
}

static void do_paint_brush_grids_cpu(const Depsgraph &depsgraph,
                                     const Sculpt &sd,
                                     Object &object,
                                     const IndexMask &node_mask)
{
  const auto flag_enabled = [](const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
  };
  if (!flag_enabled("BLENDER_SCULPT_POLYPAINT_PARALLEL_V3") &&
      !flag_enabled("BLENDER_SCULPT_POLYPAINT_UNDO_COPY_V3") &&
      !flag_enabled("BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3"))
  {
    do_paint_brush_grids_cpu_prepared_v02(depsgraph, sd, object, node_mask);
    return;
  }
  const char *enabled = std::getenv("BLENDER_SCULPT_POLYPAINT_CPU");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0') {
    return;
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  if (!ss.cache || !ss.subdiv_ccg || !ss.multires_modifier) {
    return;
  }
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double paint_start = trace ? BLI_time_now_seconds() : 0.0;
  MultiresModifierData &mmd = *ss.multires_modifier;
  SubdivCCG &ccg = *ss.subdiv_ccg;
  Mesh &base_mesh = *id_cast<Mesh *>(object.data);
  if (mmd.sculptlvl < 1 || mmd.sculptlvl > mmd.totlvl ||
      ccg.grid_size != bke::subdiv::grid_size_from_level(mmd.sculptlvl) ||
      ccg.grids_num != base_mesh.corners_num ||
      !active_color_attribute(base_mesh))
  {
    return;
  }
  const char *name = base_mesh.active_color_attribute;
  if (!name || name[0] == '\0') {
    return;
  }
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float3 primary = ss.cache->toggle_settings.invert ?
                             BKE_brush_secondary_color_get(&sd.paint, &brush) :
                             BKE_brush_color_get(&sd.paint, &brush);
  const float strength = std::clamp(
      std::abs(ss.cache->bstrength) * BKE_brush_alpha_get(&sd.paint, &brush) *
          ss.cache->paint_brush.flow,
      0.0f,
      1.0f);
  if (strength == 0.0f) {
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const char *parallel_env = std::getenv("BLENDER_SCULPT_POLYPAINT_PARALLEL_V3");
  const bool parallel_requested = parallel_env && parallel_env[0] == '1' && parallel_env[1] == '\0';
  const char *local_env = std::getenv("BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3");
  const bool local_redraw = local_env && local_env[0] == '1' && local_env[1] == '\0';
  const char *prepared_env = std::getenv("BLENDER_SCULPT_POLYPAINT_PREPARED_V2");
  const bool prepared_enabled = prepared_env && prepared_env[0] == '1' && prepared_env[1] == '\0';
  bool parallel_ok = parallel_requested && node_mask.size() > 1 &&
      mmd.sculpt_color_grids && mmd.sculpt_color_grids_num == ccg.grids_num &&
      mmd.sculpt_color_level >= mmd.sculptlvl && mmd.sculpt_color_level <= 13 &&
      (mmd.sculpt_color_level >= mmd.totlvl || bke::multires_layers::polypaint_lazy_master_enabled());
  if (parallel_ok) {
    /* Grid indices, not PBVH node ids, own the writes. Refuse overlapping
     * ownership and malformed shared payloads before any worker is started. */
    std::vector<int> owners(ccg.grids_num, -1);
    std::unordered_set<const void *> payloads;
    node_mask.foreach_index([&](const int node_index) {
      for (const int grid : nodes[node_index].grids()) {
        if (grid < 0 || grid >= ccg.grids_num) {
          parallel_ok = false;
          continue;
        }
        if (owners[grid] >= 0) {
          if (owners[grid] != node_index) parallel_ok = false;
          continue;
        }
        owners[grid] = node_index;
        const void *payload = mmd.sculpt_color_grids[grid].colors;
        if (payload && !payloads.insert(payload).second) parallel_ok = false;
      }
    });
    /* Resolve read-only mesh caches on the main thread before lazy grid
     * materialization. The directory/resolution is already fixed by stroke entry. */
    base_mesh.corner_to_face_map();
    base_mesh.faces();
    base_mesh.attributes().lookup<ColorGeometry4f>(name, bke::AttrDomain::Corner);
  }
  struct LocalWork {
    Vector<float3> positions;
    Vector<float> factors;
    Vector<float> distances;
    bke::multires_polypaint_cpu::PreparedPaintPlan prepared_plan;
    int64_t touched_samples = 0, prepared_samples = 0, prepared_grids = 0;
    double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
  };
  threading::EnumerableThreadSpecific<LocalWork> work;
  std::vector<unsigned char> node_changed(nodes.size(), 0);
  const double setup_end = trace ? BLI_time_now_seconds() : 0.0;
  auto paint_node = [&](const int node_index) {
    LocalWork &tls = work.local();
    auto &positions = tls.positions;
    auto &factors = tls.factors;
    auto &distances = tls.distances;
    auto &prepared_plan = tls.prepared_plan;
    bool changed = false;
    int64_t touched_samples = 0, prepared_samples = 0, prepared_grids = 0;
    double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
    const Span<int> grids = nodes[node_index].grids();
    double stage_start = trace ? BLI_time_now_seconds() : 0.0;
    const Span<float3> co = gather_grids_positions(ccg, grids, positions);
    if (trace) {
      const double now = BLI_time_now_seconds();
      gather_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    calc_factors_common_grids(
        depsgraph, brush, object, co, nodes[node_index], factors, distances);
    if (trace) {
      const double now = BLI_time_now_seconds();
      factors_ms += (now - stage_start) * 1000.0;
      stage_start = now;
    }
    int prepared_grid = -1;
    float (*prepared_master)[4] = nullptr;
    for (const int i : co.index_range()) {
      const float influence = std::clamp(factors[i] * strength, 0.0f, 1.0f);
      if (influence <= 0.0f) {
        continue;
      }
      const int local_grid = i / ccg.grid_area;
      const int pixel = i % ccg.grid_area;
      const int grid_index = grids[local_grid];
      const int x = pixel % ccg.grid_size;
      const int y = pixel / ccg.grid_size;
      if (prepared_grid != grid_index) {
        prepared_grid = grid_index;
        prepared_master = nullptr;
      }
      if (prepared_master) {
        const float brush_rgb[3] = {primary[0], primary[1], primary[2]};
        const bool wrote = prepared_plan.blend(prepared_master, x, y, brush_rgb, influence);
        changed |= wrote;
        touched_samples += int(wrote);
        prepared_samples += int(wrote);
        continue;
      }
      const float u = float(x) / float(ccg.grid_size - 1);
      const float v = float(y) / float(ccg.grid_size - 1);
      float before[4];
      if (!bke::multires_layers::polypaint_sample_with_base(
              mmd, base_mesh, name, grid_index, u, v, before))
      {
        continue;
      }
      float target[4];
      for (int channel = 0; channel < 3; channel++) {
        target[channel] = before[channel] * (1.0f - influence) +
                          primary[channel] * influence;
      }
      target[3] = before[3];
      const bool wrote = bke::multires_layers::polypaint_write_coarse_sample(
          mmd, base_mesh, name, grid_index, mmd.sculptlvl, x, y, target);
      if (wrote && prepared_enabled && bke::multires_layers::polypaint_grid(mmd, grid_index)) {
        const double start = trace ? BLI_time_now_seconds() : 0.0;
        const int master_side = bke::subdiv::grid_size_from_level(mmd.sculpt_color_level);
        if (prepared_plan.prepare(master_side, ccg.grid_size)) {
          prepared_master = mmd.sculpt_color_grids[grid_index].colors;
          prepared_grids++;
        }
        if (trace) prepare_ms += (BLI_time_now_seconds() - start) * 1000.0;
      }
      changed |= wrote;
      touched_samples += int(wrote);
    }
    if (trace) write_ms += (BLI_time_now_seconds() - stage_start) * 1000.0;
    tls.touched_samples += touched_samples;
    tls.prepared_samples += prepared_samples;
    tls.prepared_grids += prepared_grids;
    tls.gather_ms += gather_ms;
    tls.factors_ms += factors_ms;
    tls.write_ms += write_ms;
    tls.prepare_ms += prepare_ms;
    node_changed[node_index] = changed;
  };
  if (parallel_ok) {
    node_mask.foreach_index(paint_node, exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index(paint_node);
  }
  int64_t touched_samples = 0, prepared_samples = 0, prepared_grids = 0;
  double gather_ms = 0.0, factors_ms = 0.0, write_ms = 0.0, prepare_ms = 0.0;
  int workers = 0;
  for (const LocalWork &tls : work) {
    touched_samples += tls.touched_samples;
    prepared_samples += tls.prepared_samples;
    prepared_grids += tls.prepared_grids;
    gather_ms += tls.gather_ms;
    factors_ms += tls.factors_ms;
    write_ms += tls.write_ms;
    prepare_ms += tls.prepare_ms;
    workers++;
  }
  bool changed = false;
  node_mask.foreach_index([&](const int node_index) { changed |= bool(node_changed[node_index]); });
  const double notify_start = trace ? BLI_time_now_seconds() : 0.0;
  int64_t redraw_nodes = 0;
  if (changed) {
    if (local_redraw) {
      IndexMaskMemory memory;
      const IndexMask redraw = IndexMask::from_predicate(node_mask, memory,
          [&](const int i) { return bool(node_changed[i]); });
      pbvh.tag_attribute_changed(redraw, name);
      redraw_nodes = redraw.size();
    }
    else {
      pbvh.tag_attribute_changed(node_mask, name);
      redraw_nodes = node_mask.size();
    }
  }
  const double paint_end = trace ? BLI_time_now_seconds() : 0.0;
  if (trace) {
    std::printf("[POLYPAINT_CPU] paint_level=%d master_level=%d nodes=%lld "
                "touched=%lld changed=%d total=%.3fms\n",
                mmd.sculptlvl, mmd.sculpt_color_level,
                static_cast<long long>(node_mask.size()),
                static_cast<long long>(touched_samples), int(changed),
                (BLI_time_now_seconds() - paint_start) * 1000.0);
    std::printf("[POLYPAINT_PARALLEL_V3] requested=%d eligible=%d workers=%d redraw_local=%d redraw_nodes=%lld\n",
                int(parallel_requested), int(parallel_ok), workers, int(local_redraw),
                static_cast<long long>(redraw_nodes));
    std::printf("[POLYPAINT_PREPARED_V2] enabled=%d reused_samples=%lld prepared_grids=%lld "
                "setup=%.3fms gather=%.3fms factors=%.3fms write=%.3fms "
                "prepare=%.3fms notify=%.3fms\n",
                int(prepared_enabled), static_cast<long long>(prepared_samples),
                static_cast<long long>(prepared_grids), (setup_end - paint_start) * 1000.0,
                gather_ms, factors_ms, write_ms, prepare_ms,
                (paint_end - notify_start) * 1000.0);
  }
}

void do_paint_brush(const Depsgraph &depsgraph,
                    PaintModeSettings &paint_mode_settings,
                    const Sculpt &sd,
                    Object &ob,
                    const IndexMask &node_mask,
                    const IndexMask &texnode_mask)
{
  if (SCULPT_use_image_paint_brush(paint_mode_settings, ob)) {
    SCULPT_do_paint_brush_image(depsgraph, sd, ob, texnode_mask);
    return;
  }
  PRF_scope(ProfileCategory::Editor);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if(polypaint_v04_enabled() && color_v04_modifier(ob)) {
    do_polypaint_all_v04(depsgraph,sd,ob,node_mask);return;
  }
  if (pbvh.type() == bke::pbvh::Type::Grids) {
    do_paint_brush_grids_cpu(depsgraph, sd, ob, node_mask);
    return;
  }
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  if (!ss.cache->paint_brush.density_seed) {
    ss.cache->paint_brush.density_seed = BLI_hash_int_01(ss.cache->location_symm[0] * 1000);
  }

  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return;
  }

  float4x4 mat;

  /* If the brush is round the tip does not need to be aligned to the surface, so this saves a
   * whole iteration over the affected nodes. */
  if (brush.tip_roundness < 1.0f) {
    cube_tip_init(sd, ob, brush, mat.ptr());

    if (is_zero_m4(mat.ptr())) {
      return;
    }
  }

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }

  /* Regular Paint mode. */

  /* Wet paint color sampling. */
  float4 wet_color(0);
  if (ss.cache->paint_brush.wet_mix > 0.0f) {
    threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
    const SampleWetPaintData swptd = threading::parallel_reduce(
        node_mask.index_range(),
        1,
        SampleWetPaintData{},
        [&](const IndexRange range, SampleWetPaintData swptd) {
          ColorPaintLocalData &tls = all_tls.local();
          node_mask.slice(range).foreach_index([&](const int i) {
            do_sample_wet_paint_task(depsgraph,
                                     ob,
                                     vert_positions,
                                     vert_normals,
                                     faces,
                                     corner_verts,
                                     vert_to_face_map,
                                     attribute_data,
                                     color_attribute.span,
                                     color_attribute.domain,
                                     brush,
                                     nodes[i],
                                     tls,
                                     swptd);
          });
          return swptd;
        },
        [](const SampleWetPaintData &a, const SampleWetPaintData &b) {
          SampleWetPaintData joined{};
          joined.color = a.color + b.color;
          joined.tot_samples = a.tot_samples + b.tot_samples;
          return joined;
        });

    if (swptd.tot_samples > 0 && is_finite_v4(swptd.color)) {
      wet_color = math::clamp(swptd.color / float(swptd.tot_samples), 0.0f, 1.0f);

      if (ss.cache->first_time) {
        ss.cache->paint_brush.wet_mix_prev_color = wet_color;
      }
      blend_color_interpolate_float(wet_color,
                                    wet_color,
                                    ss.cache->paint_brush.wet_mix_prev_color,
                                    ss.cache->paint_brush.wet_persistence);
      ss.cache->paint_brush.wet_mix_prev_color = math::clamp(wet_color, 0.0f, 1.0f);
    }
  }

  if (ss.cache->paint_brush.mix_colors.is_empty()) {
    ss.cache->paint_brush.mix_colors = Array<float4>(mesh.verts_num, float4(0));
  }

  threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        ColorPaintLocalData &tls = all_tls.local();
        do_paint_brush_task(depsgraph,
                            ob,
                            vert_positions,
                            vert_normals,
                            faces,
                            corner_verts,
                            vert_to_face_map,
                            attribute_data,
                            sd.paint,
                            brush,
                            mat,
                            wet_color,
                            nodes[i],
                            tls,
                            ss.cache->paint_brush.mix_colors,
                            color_attribute);
      },
      exec_mode::grain_size(1));
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

static void do_smear_brush_task(const Depsgraph &depsgraph,
                                Object &object,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const GroupedSpan<int> vert_to_face_map,
                                const MeshAttributeData &attribute_data,
                                const Brush &brush,
                                bke::pbvh::MeshNode &node,
                                ColorPaintLocalData &tls,
                                bke::GSpanAttributeWriter &color_attribute)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  const float strength = ss.cache->bstrength;

  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide_and_mask(attribute_data.hide_vert, attribute_data.mask, verts, factors);
  filter_region_clip_factors(ss, vert_positions, verts, factors);
  if (brush.flag & BRUSH_FRONTFACE) {
    calc_front_face(cache.view_normal_symm, vert_normals, verts, factors);
  }

  tls.distances.resize(verts.size());
  const MutableSpan<float> distances = tls.distances;
  calc_brush_distances(
      ss, vert_positions, verts, eBrushFalloffShape(brush.falloff_shape), distances);
  filter_distances_with_radius(cache.radius, distances, factors);
  apply_hardness_to_distances(cache, distances);
  calc_brush_strength_factors(cache, brush, distances, factors);

  auto_mask::calc_vert_factors(depsgraph, object, cache.automasking.get(), node, verts, factors);

  calc_brush_texture_factors(ss, brush, vert_positions, verts, factors);
  scale_factors(factors, strength);

  float3 brush_delta;
  if (brush.stroke_method == BRUSH_STROKE_ANCHORED) {
    brush_delta = ss.cache->grab_delta_symm;
  }
  else {
    brush_delta = ss.cache->location_symm - ss.cache->last_location_symm;
  }

  Vector<int> neighbors;
  Vector<int> neighbor_neighbors;

  for (const int i : verts.index_range()) {
    if (factors[i] == 0.0f) {
      continue;
    }
    const int vert = verts[i];
    const float3 &no = vert_normals[vert];

    float3 current_disp;
    switch (brush.smear_deform_type) {
      case BRUSH_SMEAR_DEFORM_DRAG:
        current_disp = brush_delta;
        break;
      case BRUSH_SMEAR_DEFORM_PINCH:
        current_disp = ss.cache->location_symm - vert_positions[vert];
        break;
      case BRUSH_SMEAR_DEFORM_EXPAND:
        current_disp = vert_positions[vert] - ss.cache->location_symm;
        break;
    }

    /* Project into vertex plane. */
    current_disp += no * -math::dot(current_disp, no);

    const float3 current_disp_norm = math::normalize(current_disp);

    current_disp = current_disp_norm * strength;

    float4 accum(0);
    float totw = 0.0f;

    /*
     * NOTE: we have to do a nested iteration here to avoid
     * blocky artifacts on quad topologies.  The runtime cost
     * is not as bad as it seems due to neighbor iteration
     * in the sculpt code being cache bound; once the data is in
     * the cache iterating over it a few more times is not terribly
     * costly.
     */

    for (const int neighbor : vert_neighbors_get_mesh(
             faces, corner_verts, vert_to_face_map, attribute_data.hide_poly, vert, neighbors))
    {
      const float3 &nco = vert_positions[neighbor];
      for (const int neighbor_neighbor : vert_neighbors_get_mesh(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 attribute_data.hide_poly,
                                                                 neighbor,
                                                                 neighbor_neighbors))
      {
        if (neighbor_neighbor == vert) {
          continue;
        }

        float3 vert_disp = vert_positions[neighbor_neighbor] - vert_positions[vert];

        /* Weight by how close we are to our target distance from vd.co. */
        float w = (1.0f + fabsf(math::length(vert_disp) / strength - 1.0f));

        /* TODO: use cotangents (or at least face areas) here. */
        float len = math::distance(vert_positions[neighbor_neighbor], nco);
        if (len > 0.0f) {
          len = strength / len;
        }
        else { /* Coincident point. */
          len = 1.0f;
        }

        /* Multiply weight with edge lengths (in the future this will be
         * cotangent weights or face areas). */
        w *= len;

        /* Build directional weight. */

        /* Project into vertex plane. */
        vert_disp += no * -math::dot(no, vert_disp);
        const float3 vert_disp_norm = math::normalize(vert_disp);

        if (math::dot(current_disp_norm, vert_disp_norm) >= 0.0f) {
          continue;
        }

        const float4 &neighbor_color = ss.cache->paint_brush.prev_colors[neighbor_neighbor];
        float color_interp = -math::dot(current_disp_norm, vert_disp_norm);

        /* Square directional weight to get a somewhat sharper result. */
        w *= color_interp * color_interp;

        accum += neighbor_color * w;
        totw += w;
      }
    }

    if (totw != 0.0f) {
      accum /= totw;
    }

    float4 col = color_vert_get(
        faces, corner_verts, vert_to_face_map, color_attribute.span, color_attribute.domain, vert);
    blend_color_interpolate_float(col, ss.cache->paint_brush.prev_colors[vert], accum, factors[i]);
    color_vert_set(faces,
                   corner_verts,
                   vert_to_face_map,
                   color_attribute.domain,
                   vert,
                   col,
                   color_attribute.span);
  }
}

void do_smear_brush(const Depsgraph &depsgraph,
                    const Sculpt &sd,
                    Object &ob,
                    const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if(polypaint_v04_enabled() && color_v04_modifier(ob)) {
    do_polypaint_all_v04(depsgraph,sd,ob,node_mask);return;
  }
  if (pbvh.type()==bke::pbvh::Type::Grids) {
    do_polypaint_blur_grids_v01(depsgraph,sd,ob,node_mask,true);
    return;
  }
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (ss.cache->bstrength == 0.0f) {
    return;
  }

  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const MeshAttributeData attribute_data(mesh);

  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }

  if (ss.cache->paint_brush.prev_colors.is_empty()) {
    ss.cache->paint_brush.prev_colors = Array<float4>(mesh.verts_num);
    threading::parallel_for(IndexRange(mesh.verts_num), 1024, [&](const IndexRange range) {
      for (const int vert : range) {
        ss.cache->paint_brush.prev_colors[vert] = color_vert_get(faces,
                                                                 corner_verts,
                                                                 vert_to_face_map,
                                                                 color_attribute.span,
                                                                 color_attribute.domain,
                                                                 vert);
      }
    });
  }

  /* Smear mode. */
  node_mask.foreach_index(
      [&](const int i) {
        for (const int vert : nodes[i].verts()) {
          ss.cache->paint_brush.prev_colors[vert] = color_vert_get(faces,
                                                                   corner_verts,
                                                                   vert_to_face_map,
                                                                   color_attribute.span,
                                                                   color_attribute.domain,
                                                                   vert);
        }
      },
      exec_mode::grain_size(1));
  threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        ColorPaintLocalData &tls = all_tls.local();
        do_smear_brush_task(depsgraph,
                            ob,
                            vert_positions,
                            vert_normals,
                            faces,
                            corner_verts,
                            vert_to_face_map,
                            attribute_data,
                            brush,
                            nodes[i],
                            tls,
                            color_attribute);
      },
      exec_mode::grain_size(1));

  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

void do_blur_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &ob,
                   const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if(polypaint_v04_enabled() && color_v04_modifier(ob)) {
    do_polypaint_all_v04(depsgraph,sd,ob,node_mask);return;
  }
  if (pbvh.type()==bke::pbvh::Type::Grids) {
    do_polypaint_blur_grids_v01(depsgraph,sd,ob,node_mask);
    return;
  }
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return;
  }
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  if (!ss.cache->paint_brush.density_seed) {
    ss.cache->paint_brush.density_seed = BLI_hash_int_01(ss.cache->location_symm[0] * 1000);
  }

  if (stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    return;
  }

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const MeshAttributeData attribute_data(mesh);
  bke::GSpanAttributeWriter color_attribute = active_color_attribute_for_write(mesh);
  if (!color_attribute) {
    return;
  }

  threading::EnumerableThreadSpecific<ColorPaintLocalData> all_tls;
  node_mask.foreach_index(
      [&](const int i) {
        ColorPaintLocalData &tls = all_tls.local();
        do_color_smooth_task(depsgraph,
                             ob,
                             vert_positions,
                             vert_normals,
                             faces,
                             corner_verts,
                             vert_to_face_map,
                             attribute_data,
                             brush,
                             nodes[i],
                             tls,
                             color_attribute);
      },
      exec_mode::grain_size(1));
  pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
  color_attribute.finish();
}

}  // namespace blender::ed::sculpt_paint::color
