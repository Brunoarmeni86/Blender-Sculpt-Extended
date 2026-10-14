/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include "BLI_time.h"
#include <cmath>
#include <atomic>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_color.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_memory_utils.hh"

#include "BLO_read_write.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_texture_types.h"

#include "BKE_context.hh"
#include "MOD_util.hh"
#include "BKE_customdata.hh"
#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_paint.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subdiv_deform.hh"
#include "BKE_subdiv_mesh.hh"
#include "BKE_subdiv_foreach.hh"
#include "BKE_texture.h"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_types.hh" /* For subdivide operator UI. */

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "MOD_ui_common.hh"

namespace blender {

struct MultiresRuntimeData {
  /* Cached subdivision surface descriptor, with topology and settings. */
  bke::subdiv::Subdiv *subdiv;
};

static void init_data(ModifierData *md)
{
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(mmd, modifier);

  /* Open subdivision panels by default. */
  md->ui_expand_flag = UI_PANEL_DATA_EXPAND_ROOT | UI_SUBPANEL_DATA_EXPAND_1;
}

static void copy_data(const ModifierData *md_src, ModifierData *md_dst, const int flag)
{
  const MultiresModifierData *src = reinterpret_cast<const MultiresModifierData *>(md_src);
  MultiresModifierData *dst = reinterpret_cast<MultiresModifierData *>(md_dst);
  bke::multires_layers::clear(*dst);
  BKE_modifier_copydata_generic(md_src, md_dst, flag);
  /* Generic copying is a shallow struct copy, so discard the copied list links before
   * deep-copying the actual layer storage. */
  dst->sculpt_layers.clear_no_delete();
  bke::multires_layers::copy(*dst, *src);
}

static void free_runtime_data(void *runtime_data_v)
{
  if (runtime_data_v == nullptr) {
    return;
  }
  MultiresRuntimeData *runtime_data = static_cast<MultiresRuntimeData *>(runtime_data_v);
  if (runtime_data->subdiv != nullptr) {
    bke::subdiv::free(runtime_data->subdiv);
  }
  MEM_delete(runtime_data);
}

static void free_data(ModifierData *md)
{
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  bke::multires_layers::clear(*mmd);
  free_runtime_data(mmd->modifier.runtime);
}


static bool depends_on_time(Scene * /*scene*/, ModifierData *md)
{
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  return mmd->sculpt_displacement_texture != nullptr &&
         BKE_texture_dependsOnTime(mmd->sculpt_displacement_texture);
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  walk(user_data,
       ob,
       reinterpret_cast<ID **>(&mmd->sculpt_displacement_texture),
       IDWALK_CB_USER);
  walk(user_data, ob, reinterpret_cast<ID **>(&mmd->sculpt_shrinkwrap_target), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&mmd->sculpt_shrinkwrap_aux_target), IDWALK_CB_NOP);
  walk(user_data, ob, reinterpret_cast<ID **>(&mmd->sculpt_displacement_map_object),
       IDWALK_CB_NOP);
}

static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_masks)
{
  const auto *mmd = reinterpret_cast<const MultiresModifierData *>(md);
  if (mmd->sculpt_shrinkwrap_vgroup[0]) {
    r_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void foreach_tex_link(ModifierData *md, Object *ob, TexWalkFunc walk, void *user_data)
{
  PointerRNA ptr = RNA_pointer_create_discrete(&ob->id, RNA_Modifier, md);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "sculpt_displacement_texture");
  walk(user_data, ob, md, &ptr, prop);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  if (mmd->sculpt_displacement_map_object && mmd->sculpt_displacement_mapping == 3) {
    MOD_depsgraph_update_object_bone_relation(ctx->node, mmd->sculpt_displacement_map_object,
        mmd->sculpt_displacement_map_bone, "Multires Texture Coordinates");
  }
  if (ELEM(mmd->sculpt_displacement_mapping, 2, 3)) {
    DEG_add_depends_on_transform_relation(ctx->node, "Multires Texture Coordinates");
  }
  if (mmd->sculpt_displacement_texture != nullptr) {
    DEG_add_generic_id_relation(
        ctx->node, &mmd->sculpt_displacement_texture->id, "Multires Texture Displacement");
  }
  for (Object *target : {mmd->sculpt_shrinkwrap_target, mmd->sculpt_shrinkwrap_aux_target}) {
    if (target && target != ctx->object) {
      DEG_add_object_relation(ctx->node, target, DEG_OB_COMP_TRANSFORM, "Multires Shrinkwrap");
      DEG_add_object_relation(ctx->node, target, DEG_OB_COMP_GEOMETRY, "Multires Shrinkwrap");
      if (mmd->sculpt_shrinkwrap_method == MOD_SHRINKWRAP_TARGET_PROJECT) {
        DEG_add_special_eval_flag(ctx->node, &target->id, DAG_EVAL_NEED_SHRINKWRAP_BOUNDARY);
      }
    }
  }
  DEG_add_depends_on_transform_relation(ctx->node, "Multires Shrinkwrap");
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const MultiresModifierData *mmd = reinterpret_cast<const MultiresModifierData *>(md);
  writer->write_struct(mmd);
  writer->write_struct_list(&mmd->sculpt_layers);
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (layer.base_deltas && layer.base_verts_num > 0) {
      writer->write_raw(sizeof(float) * size_t(layer.base_verts_num) * 3, layer.base_deltas);
    }
    if (layer.disps == nullptr || layer.grids_num <= 0) {
      continue;
    }
    writer->write_struct_array(layer.grids_num, layer.disps);
    for (int i = 0; i < layer.grids_num; i++) {
      const MDisps &grid = layer.disps[i];
      if (grid.disps != nullptr && grid.totdisp > 0) {
        writer->write_raw(sizeof(float) * size_t(grid.totdisp) * 3, grid.disps);
      }
    }
  }
  if (mmd->sculpt_color_grids && mmd->sculpt_color_grids_num > 0) {
    writer->write_struct_array(mmd->sculpt_color_grids_num, mmd->sculpt_color_grids);
    for (int i = 0; i < mmd->sculpt_color_grids_num; i++) {
      const MultiresColorGrid &grid = mmd->sculpt_color_grids[i];
      if (grid.colors && grid.totcolor > 0) {
        writer->write_raw(sizeof(float) * size_t(grid.totcolor) * 4, grid.colors);
      }
    }
  }
  writer->write_struct_list(&mmd->sculpt_color_attributes);
  for (const MultiresColorAttribute &attribute : mmd->sculpt_color_attributes) {
    if (attribute.grids && attribute.grids_num > 0) {
      writer->write_struct_array(attribute.grids_num, attribute.grids);
      for (int i = 0; i < attribute.grids_num; i++) {
        const MultiresColorGrid &grid = attribute.grids[i];
        if (grid.colors && grid.totcolor > 0) {
          writer->write_raw(sizeof(float) * size_t(grid.totcolor) * 4, grid.colors);
        }
      }
    }
  }
  if (mmd->sculpt_face_set_grids && mmd->sculpt_face_set_grids_num > 0) {
    writer->write_struct_array(mmd->sculpt_face_set_grids_num, mmd->sculpt_face_set_grids);
    for (int i = 0; i < mmd->sculpt_face_set_grids_num; i++) {
      const MultiresFaceSetGrid &grid = mmd->sculpt_face_set_grids[i];
      if (grid.ids && grid.totid > 0) {
        writer->write_raw(sizeof(int) * size_t(grid.totid), grid.ids);
      }
    }
  }
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  BLO_read_struct_list(reader, MultiresSculptLayer, &mmd->sculpt_layers);
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (layer.base_deltas && layer.base_verts_num > 0) {
      if (!BLO_read_array(reader, reinterpret_cast<float **>(&layer.base_deltas),
                          layer.base_verts_num, 3)) {
        layer.base_deltas = nullptr;
        layer.base_verts_num = 0;
      }
    }
    else {
      layer.base_deltas = nullptr;
      layer.base_verts_num = 0;
    }
    if (layer.grids_num <= 0 || layer.disps == nullptr) {
      continue;
    }
    if (!BLO_read_array(reader, &layer.disps, layer.grids_num)) {
      layer.disps = nullptr;
      layer.grids_num = 0;
      continue;
    }
    for (int i = 0; i < layer.grids_num; i++) {
      MDisps &grid = layer.disps[i];
      if (grid.totdisp <= 0 || grid.disps == nullptr) {
        continue;
      }
      if (!BLO_read_array(reader, reinterpret_cast<float **>(&grid.disps), grid.totdisp, 3)) {
        grid.disps = nullptr;
        grid.totdisp = 0;
      }
    }
  }
  if (mmd->sculpt_color_grids_num > 0 && mmd->sculpt_color_grids) {
    if (!BLO_read_array(reader, &mmd->sculpt_color_grids, mmd->sculpt_color_grids_num)) {
      mmd->sculpt_color_grids = nullptr;
      mmd->sculpt_color_grids_num = 0;
      mmd->sculpt_color_level = 0;
    }
    else {
      for (int i = 0; i < mmd->sculpt_color_grids_num; i++) {
        MultiresColorGrid &grid = mmd->sculpt_color_grids[i];
        if (grid.totcolor > 0 && grid.colors &&
            !BLO_read_array(reader, reinterpret_cast<float **>(&grid.colors), grid.totcolor, 4))
        {
          grid.colors = nullptr;
          grid.totcolor = 0;
        }
      }
    }
  }
  BLO_read_struct_list(reader, MultiresColorAttribute, &mmd->sculpt_color_attributes);
  mmd->sculpt_color_attribute[sizeof(mmd->sculpt_color_attribute) - 1] = '\0';
  for (MultiresColorAttribute &attribute : mmd->sculpt_color_attributes) {
    attribute.name[sizeof(attribute.name) - 1] = '\0';
    if (attribute.grids_num <= 0 || !attribute.grids ||
        !BLO_read_array(reader, &attribute.grids, attribute.grids_num)) {
      attribute.grids = nullptr;
      attribute.grids_num = 0;
      attribute.level = 0;
      continue;
    }
    for (int i = 0; i < attribute.grids_num; i++) {
      MultiresColorGrid &grid = attribute.grids[i];
      if (grid.totcolor > 0 && grid.colors &&
          !BLO_read_array(reader, reinterpret_cast<float **>(&grid.colors), grid.totcolor, 4)) {
        grid.colors = nullptr;
        grid.totcolor = 0;
      }
    }
  }
  if (mmd->sculpt_face_set_grids_num > 0 && mmd->sculpt_face_set_grids) {
    if (!BLO_read_array(reader, &mmd->sculpt_face_set_grids, mmd->sculpt_face_set_grids_num)) {
      mmd->sculpt_face_set_grids = nullptr;
      mmd->sculpt_face_set_grids_num = 0;
      mmd->sculpt_face_set_level = 0;
    }
    else {
      for (int i = 0; i < mmd->sculpt_face_set_grids_num; i++) {
        MultiresFaceSetGrid &grid = mmd->sculpt_face_set_grids[i];
        if (grid.totid > 0 && grid.ids &&
            !BLO_read_array(reader, &grid.ids, grid.totid))
        {
          grid.ids = nullptr;
          grid.totid = 0;
        }
      }
    }
  }
}


static MultiresRuntimeData *multires_ensure_runtime(MultiresModifierData *mmd)
{
  MultiresRuntimeData *runtime_data = static_cast<MultiresRuntimeData *>(mmd->modifier.runtime);
  if (runtime_data == nullptr) {
    runtime_data = MEM_new_zeroed<MultiresRuntimeData>(__func__);
    mmd->modifier.runtime = runtime_data;
  }
  return runtime_data;
}

/* Main goal of this function is to give usable subdivision surface descriptor
 * which matches settings and topology. */
static bke::subdiv::Subdiv *subdiv_descriptor_ensure(MultiresModifierData *mmd,
                                                     const bke::subdiv::Settings *subdiv_settings,
                                                     const Mesh *mesh)
{
  MultiresRuntimeData *runtime_data = static_cast<MultiresRuntimeData *>(mmd->modifier.runtime);
  bke::subdiv::Subdiv *subdiv = bke::subdiv::update_from_mesh(
      runtime_data->subdiv, subdiv_settings, mesh);
  runtime_data->subdiv = subdiv;
  return subdiv;
}

/* Subdivide into fully qualified mesh. */

/* POLYPAINT_EVALUATED_MESH_COLOR_V1: persistent colors stay in the original
 * Multires modifier. Object Mode / render consume a read-only projection into
 * the named color attribute of the evaluated mesh, independently of PBVH. */
static const MultiresModifierData *polypaint_mesh_color_source(
    const MultiresModifierData &mmd, const ModifierEvalContext &ctx, const Mesh &base_mesh)
{
  const char *enabled = std::getenv("BLENDER_SCULPT_POLYPAINT_MESH_COLOR");
  if (!enabled || enabled[0] != '1' || enabled[1] != '\0' ||
      (ctx.flag & MOD_APPLY_ORCO) ||
      !base_mesh.active_color_attribute || !base_mesh.active_color_attribute[0])
  {
    return nullptr;
  }
  const MultiresModifierData *source = &mmd;
  ModifierData *original = BKE_modifier_get_original(
      ctx.object, const_cast<ModifierData *>(&mmd.modifier));
  if (original && original->type == eModifierType_Multires) {
    source = reinterpret_cast<const MultiresModifierData *>(original);
  }
  if (!source->sculpt_color_attributes.is_empty()) {
    return source;
  }
  return source->sculpt_color_grids && source->sculpt_color_level >= 1 &&
                 source->sculpt_color_level <= 13 &&
                 source->sculpt_color_grids_num == base_mesh.corners_num ?
             source : nullptr;
}

static bke::SpanAttributeWriter<ColorGeometry4f> polypaint_mesh_color_writer(
    Mesh &mesh, const std::string &name)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const std::optional<bke::AttributeMetaData> meta = attributes.lookup_meta_data(name);
  if (!bke::mesh::is_color_attribute(meta)) {
    return {};
  }
  if (meta->domain == bke::AttrDomain::Corner && meta->data_type == bke::AttrType::ColorFloat) {
    return attributes.lookup_for_write_span<ColorGeometry4f>(name);
  }
  /* Preserve subdivision's own interpolation on all unpainted corners. A
   * temporary attribute converts BYTE/POINT to float CORNER directly, without
   * a second full-size staging color array. Attribute names on the mesh remain
   * valid because only the storage entry is replaced, after materialization. */
  const VArray<ColorGeometry4f> inherited =
      *attributes.lookup<ColorGeometry4f>(name, bke::AttrDomain::Corner);
  if (!inherited || inherited.size() != mesh.corners_num) {
    return {};
  }
  std::string temporary = ".polypaint_eval_color";
  while (attributes.contains(temporary)) {
    temporary += "_";
  }
  bke::SpanAttributeWriter<ColorGeometry4f> converted =
      attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(temporary,
                                                                   bke::AttrDomain::Corner);
  if (!converted) {
    return {};
  }
  inherited.materialize(converted.span);
  converted.finish();
  if (!attributes.rename(temporary, name, true)) {
    attributes.remove(temporary);
    return {};
  }
  return attributes.lookup_for_write_span<ColorGeometry4f>(name);
}

static bool polypaint_mesh_sample(const MultiresModifierData &source,
                                 const OffsetIndices<int> faces,
                                 const int face_index,
                                 const int coarse_corner,
                                 const float u,
                                 const float v,
                                 float rgba[4])
{
  if (face_index < 0 || face_index >= faces.size() || !std::isfinite(u) || !std::isfinite(v)) {
    return false;
  }
  const IndexRange face = faces[face_index];
  if (face.size() < 3) {
    return false;
  }
  float corner_u = std::clamp(u, 0.0f, 1.0f);
  float corner_v = std::clamp(v, 0.0f, 1.0f);
  int corner = coarse_corner;
  if (face.size() == 4) {
    corner = bke::subdiv::rotate_quad_to_corner(corner_u, corner_v, &corner_u, &corner_v);
  }
  if (corner < 0 || corner >= face.size()) {
    return false;
  }
  float grid_u, grid_v;
  bke::subdiv::ptex_face_uv_to_grid_uv(corner_u, corner_v, &grid_u, &grid_v);
  if (!bke::multires_layers::polypaint_sample(
          source, face.start() + corner, grid_u, grid_v, rgba))
  {
    return false;
  }
  for (int c = 0; c < 4; c++) {
    if (!std::isfinite(rgba[c])) {
      return false;
    }
  }
  return true;
}

static void polypaint_evaluated_mesh_attribute(const MultiresModifierData &source,
                                         const char *attribute_name,
                                         const Mesh &base_mesh,
                                         Mesh &evaluated_mesh,
                                         bke::subdiv::Subdiv *subdiv,
                                         const bke::subdiv::ToMeshSettings *settings)
{
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double start = trace ? BLI_time_now_seconds() : 0.0;
  const std::string name(attribute_name);
  bke::SpanAttributeWriter<ColorGeometry4f> colors =
      polypaint_mesh_color_writer(evaluated_mesh, name);
  if (!colors) {
    if (trace) {
      std::printf("[POLYPAINT_MESH_COLOR] mode=SKIP attribute=%s reason=ATTRIBUTE_MISSING_OR_INVALID\n",
                  name.c_str());
    }
    return;
  }
  struct UserData {
    const MultiresModifierData *source;
    OffsetIndices<int> faces;
    MutableSpan<ColorGeometry4f> colors;
    std::atomic<int64_t> samples{0};
  } data{&source, base_mesh.faces(), colors.span};
  bool traversed = true;
  if (subdiv && settings) {
    bke::subdiv::ForeachContext context{};
    context.user_data = &data;
    context.loop = [](const bke::subdiv::ForeachContext *ctx, void *, int,
                      const float u, const float v, int, const int face,
                      const int coarse_corner, const int loop, int, int) {
      auto &data = *static_cast<UserData *>(ctx->user_data);
      if (loop < 0 || loop >= data.colors.size()) {
        return;
      }
      float rgba[4];
      if (polypaint_mesh_sample(*data.source, data.faces, face, coarse_corner, u, v, rgba)) {
        ColorGeometry4f &color = data.colors[loop];
        color.r = rgba[0]; color.g = rgba[1]; color.b = rgba[2]; color.a = rgba[3];
        data.samples.fetch_add(1, std::memory_order_relaxed);
      }
    };
    traversed = bke::subdiv::foreach_subdiv_geometry(subdiv, &context, settings, &base_mesh);
  }
  else {
    /* Viewport/Render level 0 has one result corner per coarse corner. Sample
     * the corresponding original-corner endpoint of the preserved master. */
    threading::parallel_for(IndexRange(base_mesh.corners_num), 4096, [&](const IndexRange range) {
      int64_t samples = 0;
      for (const int corner : range) {
        float rgba[4];
        if (bke::multires_layers::polypaint_sample(source, corner, 1.0f, 1.0f, rgba) &&
            std::isfinite(rgba[0]) && std::isfinite(rgba[1]) &&
            std::isfinite(rgba[2]) && std::isfinite(rgba[3]))
        {
          ColorGeometry4f &color = data.colors[corner];
          color.r = rgba[0]; color.g = rgba[1]; color.b = rgba[2]; color.a = rgba[3];
          samples++;
        }
      }
      data.samples.fetch_add(samples, std::memory_order_relaxed);
    });
  }
  colors.finish();
  if (trace) {
    std::printf("[POLYPAINT_MESH_COLOR] mode=%s attribute=%s corners=%d sampled=%lld "
                "complete=%d total=%.3fms\n",
                subdiv ? "SUBDIV" : "BASE", name.c_str(), evaluated_mesh.corners_num,
                static_cast<long long>(data.samples.load(std::memory_order_relaxed)),
                int(traversed), (BLI_time_now_seconds() - start) * 1000.0);
  }
}

static void polypaint_evaluated_mesh_color(const MultiresModifierData &source,
                                         const Mesh &base_mesh, Mesh &evaluated_mesh,
                                         bke::subdiv::Subdiv *subdiv,
                                         const bke::subdiv::ToMeshSettings *settings)
{
  const char *active_name = source.sculpt_color_attribute[0] ?
      source.sculpt_color_attribute : base_mesh.active_color_attribute;
  if (active_name && active_name[0] && source.sculpt_color_grids) {
    polypaint_evaluated_mesh_attribute(source, active_name, base_mesh, evaluated_mesh, subdiv, settings);
  }
  for (const MultiresColorAttribute &attribute : source.sculpt_color_attributes) {
    MultiresModifierData view{};
    if (attribute.grids && bke::multires_layers::polypaint_attribute_view(source, attribute.name, view)) {
      polypaint_evaluated_mesh_attribute(view, attribute.name, base_mesh, evaluated_mesh, subdiv, settings);
    }
  }
}


/* Use the same mask evaluator and subdivision vertex indexing as CCG. This keeps
 * Object/Render evaluation and Bake consistent with the live Sculpt preview. */
static Array<float> multires_shrinkwrap_mesh_masks(bke::subdiv::Subdiv &subdiv,
                                                 const bke::subdiv::ToMeshSettings &settings,
                                                 const Mesh &base_mesh,
                                                 const int verts_num)
{
  SubdivCCGMaskEvaluator evaluator{};
  if (!BKE_subdiv_ccg_mask_init_from_paint(&evaluator, &base_mesh)) {
    return {};
  }
  Array<float> masks(verts_num, 0.0f);
  struct UserData {
    SubdivCCGMaskEvaluator *evaluator;
    MutableSpan<float> masks;
  } data{&evaluator, masks};
  bke::subdiv::ForeachContext context{};
  context.user_data = &data;
  context.vert_corner = context.vert_edge = [](
      const bke::subdiv::ForeachContext *ctx, void *, const int ptex, const float u,
      const float v, int, int, int, const int vert) {
    auto &data = *static_cast<UserData *>(ctx->user_data);
    data.masks[vert] = data.evaluator->eval_mask(data.evaluator, ptex, u, v);
  };
  context.vert_inner = [](
      const bke::subdiv::ForeachContext *ctx, void *, const int ptex, const float u,
      const float v, int, int, const int vert) {
    auto &data = *static_cast<UserData *>(ctx->user_data);
    data.masks[vert] = data.evaluator->eval_mask(data.evaluator, ptex, u, v);
  };
  bke::subdiv::foreach_subdiv_geometry(&subdiv, &context, &settings, &base_mesh);
  evaluator.free(&evaluator);
  return masks;
}

static Mesh *multires_as_mesh(const MultiresModifierData *mmd,
                              const ModifierEvalContext *ctx,
                              Mesh *mesh,
                              bke::subdiv::Subdiv *subdiv)
{
  Mesh *result = mesh;
  const bool use_render_params = (ctx->flag & MOD_APPLY_RENDER);
  const bool ignore_simplify = (ctx->flag & MOD_APPLY_IGNORE_SIMPLIFY);
  const bool ignore_control_edges = (ctx->flag & MOD_APPLY_TO_ORIGINAL);
  const Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
  Object *object = ctx->object;
  bke::subdiv::ToMeshSettings mesh_settings;
  BKE_multires_subdiv_mesh_settings_init(&mesh_settings,
                                         scene,
                                         object,
                                         mmd,
                                         use_render_params,
                                         ignore_simplify,
                                         ignore_control_edges);
  if (mesh_settings.resolution < 3) {
    return result;
  }
  bke::subdiv::displacement_attach_from_multires(
      subdiv, mesh, mmd, multires_texture_mapping_matrix(*mmd, *ctx->object));
  result = bke::subdiv::subdiv_to_mesh(subdiv, &mesh_settings, mesh);
  if (result != nullptr && result != mesh) {
    if (const MultiresModifierData *source = polypaint_mesh_color_source(*mmd, *ctx, *mesh)) {
      polypaint_evaluated_mesh_color(*source, *mesh, *result, subdiv, &mesh_settings);
    }
    if (mmd->sculpt_shrinkwrap_enabled) {
      const Array<float> masks = multires_shrinkwrap_mesh_masks(
          *subdiv, mesh_settings, *mesh, result->verts_num);
      bke::multires_layers::shrinkwrap_mesh(
          ctx->depsgraph, *ctx->object, *mmd, *result, true, masks);
    }
  }
  return result;
}

/* Subdivide into CCG. */

static void multires_ccg_settings_init(SubdivToCCGSettings *settings,
                                       const MultiresModifierData *mmd,
                                       const ModifierEvalContext *ctx,
                                       const Mesh *mesh)
{
  const bool has_mask = CustomData_has_layer(&mesh->corner_data, CD_GRID_PAINT_MASK);
  const bool use_render_params = (ctx->flag & MOD_APPLY_RENDER);
  const bool ignore_simplify = (ctx->flag & MOD_APPLY_IGNORE_SIMPLIFY);
  const Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
  Object *object = ctx->object;
  const int level = multires_get_level(scene, object, mmd, use_render_params, ignore_simplify);
  settings->resolution = (1 << level) + 1;
  settings->need_normal = true;
  settings->need_mask = has_mask;
}

static Mesh *multires_as_ccg(MultiresModifierData *mmd,
                             const ModifierEvalContext *ctx,
                             Mesh *mesh,
                             bke::subdiv::Subdiv *subdiv)
{
  Mesh *result = mesh;
  SubdivToCCGSettings ccg_settings;
  multires_ccg_settings_init(&ccg_settings, mmd, ctx, mesh);
  if (ccg_settings.resolution < 3) {
    return result;
  }
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double attach_start = trace ? BLI_time_now_seconds() : 0.0;
  bke::subdiv::displacement_attach_from_multires(
      subdiv, mesh, mmd, multires_texture_mapping_matrix(*mmd, *ctx->object));
  if (trace) {
    std::printf("[MULTIRES_DISPLACEMENT_ATTACH] total=%.3fms\n",
                (BLI_time_now_seconds() - attach_start) * 1000.0);
  }
  const double ccg_start = trace ? BLI_time_now_seconds() : 0.0;
  result = BKE_subdiv_to_ccg_mesh(*subdiv, ccg_settings, *mesh);
  if (trace) {
    std::printf("[MULTIRES_TO_CCG] faces=%d corners=%d total=%.3fms\n",
                mesh->faces_num, mesh->corners_num,
                (BLI_time_now_seconds() - ccg_start) * 1000.0);
  }
  if (result && result->runtime->subdiv_ccg) {
    SubdivCCG &ccg = *result->runtime->subdiv_ccg;
    if (bke::multires_layers::shrinkwrap_ccg(
            ctx->depsgraph, *ctx->object, *mmd, *mesh, ccg))
    {
      BKE_subdiv_ccg_average_grids(ccg);
      BKE_subdiv_ccg_recalc_normals(ccg);
    }
  }

  /* NOTE: CCG becomes an owner of Subdiv descriptor, so can not share
   * this pointer. Not sure if it's needed, but might have a second look
   * on the ownership model here. */
  MultiresRuntimeData *runtime_data = static_cast<MultiresRuntimeData *>(mmd->modifier.runtime);
  runtime_data->subdiv = nullptr;

  return result;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  /* LAYER_VISIBLE_EVAL_TRACE_V2 */
  const char *layer_trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool layer_trace = layer_trace_env && layer_trace_env[0] && layer_trace_env[0] != '0';
  const double layer_eval_start = layer_trace ? BLI_time_now_seconds() : 0.0;
  BLI_SCOPED_DEFER([&]() {
    if (layer_trace) {
      std::printf("[MULTIRES_MODIFIER_EVAL] sculpt=%d total=%.3fms\n",
                  int((ctx->object->mode & OB_MODE_SCULPT) != 0),
                  (BLI_time_now_seconds() - layer_eval_start) * 1000.0);
    }
  });
  Mesh *result = mesh;
#if !defined(WITH_OPENSUBDIV)
  BKE_modifier_set_error(ctx->object, md, "Disabled, built without OpenSubdiv");
  return result;
#endif
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  Mesh *cage_copy = nullptr;
  bool use_base_deltas = false;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    use_base_deltas |= layer.base_deltas && (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE);
  }
  if (use_base_deltas && !((ctx->object->mode & OB_MODE_SCULPT) &&
                          (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh))) {
    cage_copy = BKE_mesh_copy_for_eval(*mesh);
    if (!bke::multires_layers::base_deltas_apply(*mmd, cage_copy->vert_positions_for_write())) {
      BKE_id_free(nullptr, cage_copy);
      BKE_modifier_set_error(ctx->object, md, "Invalid L0 layer data; base mesh retained");
      return result;
    }
    cage_copy->tag_positions_changed();
    mesh = cage_copy;
    result = mesh;
  }
  BLI_SCOPED_DEFER([&]() {
    if (cage_copy && result != cage_copy) {
      BKE_id_free(nullptr, cage_copy);
    }
  });
  bke::subdiv::Settings subdiv_settings;
  BKE_multires_subdiv_settings_init(&subdiv_settings, mmd);
  const Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
  const bool use_render = ctx->flag & MOD_APPLY_RENDER;
  const bool ignore_simplify = ctx->flag & MOD_APPLY_IGNORE_SIMPLIFY;
  if (subdiv_settings.level == 0 ||
      multires_get_level(scene, ctx->object, mmd, use_render, ignore_simplify) == 0) {
    if (const MultiresModifierData *source = polypaint_mesh_color_source(*mmd, *ctx, *mesh)) {
      if (!cage_copy) {
        cage_copy = BKE_mesh_copy_for_eval(*mesh);
        result = cage_copy;
      }
      polypaint_evaluated_mesh_color(*source, *mesh, *result, nullptr, nullptr);
    }
    if (mmd->sculpt_shrinkwrap_enabled && mmd->sculpt_shrinkwrap_target &&
        !((ctx->object->mode & OB_MODE_SCULPT) &&
          (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh))) {
      if (!cage_copy) {
        cage_copy = BKE_mesh_copy_for_eval(*mesh);
        result = cage_copy;
      }
      bke::multires_layers::shrinkwrap_mesh(ctx->depsgraph, *ctx->object, *mmd, *result);
    }
    return result;
  }
  MultiresRuntimeData *runtime_data = multires_ensure_runtime(mmd);
  const double descriptor_start = layer_trace ? BLI_time_now_seconds() : 0.0;
  bke::subdiv::Subdiv *subdiv = subdiv_descriptor_ensure(mmd, &subdiv_settings, mesh);
  if (layer_trace) {
    std::printf("[MULTIRES_DESCRIPTOR] faces=%d corners=%d total=%.3fms\n",
                mesh->faces_num, mesh->corners_num,
                (BLI_time_now_seconds() - descriptor_start) * 1000.0);
  }
  if (subdiv == nullptr) {
    /* Happens on bad topology, also on empty input mesh. */
    return result;
  }
  const bool use_clnors = mmd->flags & eMultiresModifierFlag_UseCustomNormals &&
                          mesh->normals_domain() == bke::MeshNormalDomain::Corner;
  /* NOTE: Orco needs final coordinates on CPU side, which are expected to be
   * accessible via mesh vertices. For this reason we do not evaluate multires to
   * grids when orco is requested. */
  const bool for_orco = (ctx->flag & MOD_APPLY_ORCO) != 0;
  /* Needed when rendering or baking will in sculpt mode. */
  const bool for_render = (ctx->flag & MOD_APPLY_RENDER) != 0;

  const bool sculpt_base_mesh = mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh;

  if ((ctx->object->mode & OB_MODE_SCULPT) && !for_orco && !for_render && !sculpt_base_mesh) {
    /* NOTE: CCG takes ownership over Subdiv. */
    result = multires_as_ccg(mmd, ctx, mesh, subdiv);
    result->runtime->subdiv_ccg_tot_level = mmd->totlvl;
    /* TODO(sergey): Usually it is sculpt stroke's update variants which
     * takes care of this, but is possible that we need this before the
     * stroke: i.e. when exiting blender right after stroke is done.
     * Annoying and not so much black-boxed as far as sculpting goes, and
     * surely there is a better way of solving this. */
    if (ctx->object->runtime->sculpt_session != nullptr) {
      SculptSession *sculpt_session = ctx->object->runtime->sculpt_session;
      sculpt_session->subdiv_ccg = result->runtime->subdiv_ccg.get();
      sculpt_session->multires_modifier = mmd;
    }
    // bke::subdiv::stats_print(&subdiv->stats);
  }
  else {
    if (use_clnors) {
      void *data = CustomData_add_layer(
          &mesh->corner_data, CD_NORMAL, CD_CONSTRUCT, mesh->corners_num);
      memcpy(data, mesh->corner_normals().data(), mesh->corner_normals().size_in_bytes());
    }

    result = multires_as_mesh(mmd, ctx, mesh, subdiv);

    if (use_clnors) {
      bke::mesh_set_custom_normals_normalized(
          *result,
          {static_cast<float3 *>(CustomData_get_layer_for_write(
               &result->corner_data, CD_NORMAL, result->corners_num)),
           result->corners_num});
      CustomData_free_layers(&result->corner_data, CD_NORMAL);
    }
    // bke::subdiv::stats_print(&subdiv->stats);
    if (subdiv != runtime_data->subdiv) {
      bke::subdiv::free(subdiv);
    }
  }
  return result;
}

static void deform_matrices(ModifierData *md,
                            const ModifierEvalContext *ctx,
                            Mesh *mesh,
                            MutableSpan<float3> positions,
                            MutableSpan<float3x3> /*matrices*/)

{
#if !defined(WITH_OPENSUBDIV)
  BKE_modifier_set_error(ctx->object, md, "Disabled, built without OpenSubdiv");
  return;
#endif

  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);

  bke::subdiv::Settings subdiv_settings;
  BKE_multires_subdiv_settings_init(&subdiv_settings, mmd);
  if (subdiv_settings.level == 0) {
    return;
  }

  SubdivToCCGSettings ccg_settings;
  multires_ccg_settings_init(&ccg_settings, mmd, ctx, mesh);
  if (ccg_settings.resolution < 3) {
    return;
  }

  MultiresRuntimeData *runtime_data = multires_ensure_runtime(mmd);
  bke::subdiv::Subdiv *subdiv = subdiv_descriptor_ensure(mmd, &subdiv_settings, mesh);
  if (subdiv == nullptr) {
    /* Happens on bad topology, also on empty input mesh. */
    return;
  }
  bke::subdiv::displacement_attach_from_multires(
      subdiv, mesh, mmd, multires_texture_mapping_matrix(*mmd, *ctx->object));
  bke::subdiv::deform_coarse_vertices(subdiv, mesh, positions);
  if (subdiv != runtime_data->subdiv) {
    bke::subdiv::free(subdiv);
  }
}

static void panel_draw(const bContext *C, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  MultiresModifierData *mmd = static_cast<MultiresModifierData *>(ptr->data);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(true);
  col.prop(ptr, "levels", UI_ITEM_NONE, IFACE_("Levels Viewport"), ICON_NONE);
  col.prop(ptr, "sculpt_levels", UI_ITEM_NONE, IFACE_("Sculpt"), ICON_NONE);
  col.prop(ptr, "render_levels", UI_ITEM_NONE, IFACE_("Render"), ICON_NONE);

  const bool is_sculpt_mode = CTX_data_active_object(C)->mode & OB_MODE_SCULPT;
  ui::Block *block = layout.block();
  block_lock_set(block, !is_sculpt_mode, N_("Sculpt Base Mesh"));
  col.prop(ptr, "use_sculpt_base_mesh", UI_ITEM_NONE, IFACE_("Sculpt Base Mesh"), ICON_NONE);
  block_lock_clear(block);

  layout.prop(ptr, "show_only_control_edges", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  /* Persistent sculpt layers. Keep this compact in the modifier panel; the same data is also
   * exposed through RNA for scripts and for a future dedicated Sculpt Layers editor. */
  {
    layout.separator();
    layout.label(IFACE_("Sculpt Layers"), ICON_MOD_MULTIRES);

    ui::Layout &base_box = layout.box();
    base_box.label(IFACE_("Base - native Multires surface"), ICON_MOD_MULTIRES);
    base_box.label(IFACE_("Stored separately from sculpt layers"), ICON_NONE);
    base_box.op("SCULPT_OT_multires_base_capture", IFACE_("Bake Visible into Base..."), ICON_NONE);

    if (const MultiresSculptLayer *active = bke::multires_layers::active(*mmd)) {
      /* REC L0: allow recording base deltas at sculpt level zero.
       * Level zero with REC off still edits the real base mesh. Sculpt Base Mesh
       * mode remains a separate explicit base-edit mode and keeps REC disabled. */
      const bool sculpt_base_mesh =
          (mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh) != 0;
      const bool editing_base =
          sculpt_base_mesh || (mmd->sculptlvl == 0 && !mmd->sculpt_layer_recording);
      ui::Layout &active_box = layout.box();
      ui::Layout &record_row = active_box.row(false);
      record_row.enabled_set(!sculpt_base_mesh);
      record_row.prop(ptr, "sculpt_layer_recording", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      if (editing_base) {
        active_box.label(IFACE_("Editing base mesh coordinates"), ICON_INFO);
      }
      active_box.label(!editing_base && mmd->sculpt_layer_recording ? IFACE_("Recording into:") :
                                                  IFACE_("Selected layer (REC off):"), ICON_RADIOBUT_ON);
      active_box.label(active->name, ICON_NONE);
      active_box.op("SCULPT_OT_multires_layer_commit", IFACE_("Commit Pending Changes"), ICON_NONE);
      active_box.op("SCULPT_OT_multires_layer_snapshot", IFACE_("Create Hidden Copy"), ICON_DUPLICATE);
      active_box.label(IFACE_("Save the .blend to keep changes on disk"), ICON_NONE);
      if (!editing_base && (!(active->flag & MULTIRES_SCULPT_LAYER_VISIBLE) ||
          !std::isfinite(active->strength) || std::abs(active->strength) < 1e-6f ||
          mmd->sculptlvl > mmd->totlvl)) {
        active_box.label(IFACE_("Recording requires a valid sculpt level and a visible layer with non-zero strength"), ICON_ERROR);
      }
    }

    ui::Layout &layer_tools = layout.row(false);
    layer_tools.op("SCULPT_OT_multires_layer_add", IFACE_("Add"), ICON_ADD);
    layer_tools.op("SCULPT_OT_multires_layer_duplicate", IFACE_("Duplicate"), ICON_DUPLICATE);
    layer_tools.op("SCULPT_OT_multires_layer_delete", IFACE_("Delete"), ICON_X);
    PointerRNA move_up = layer_tools.op(
        "SCULPT_OT_multires_layer_move", IFACE_("Up"), ICON_TRIA_UP);
    RNA_enum_set(&move_up, "direction", -1);
    PointerRNA move_down = layer_tools.op(
        "SCULPT_OT_multires_layer_move", IFACE_("Down"), ICON_TRIA_DOWN);
    RNA_enum_set(&move_down, "direction", 1);
    ui::Layout &group_tools = layout.row(false);
    group_tools.op("SCULPT_OT_multires_layer_group_create", IFACE_("New Group"), ICON_FILE_FOLDER);
group_tools.op("SCULPT_OT_multires_layer_group_remove", IFACE_("Ungroup"), ICON_X);
    layer_tools.op("SCULPT_OT_multires_layer_merge", IFACE_("Merge Down"), ICON_TRIA_DOWN);
    layer_tools.op("SCULPT_OT_multires_layer_flatten", IFACE_("Flatten"), ICON_TRIA_DOWN);

    int last_sculpt_group_id = 0;
    for (int i = 0; i < mmd->sculpt_layers.count(); i++) {
      MultiresSculptLayer *layer = static_cast<MultiresSculptLayer *>(
          BLI_findlink(&mmd->sculpt_layers, i));
      if (layer == nullptr) {
        continue;
      }
      if (layer->group_id != 0 && layer->group_id != last_sculpt_group_id) {
        const bool group_hidden =
            (layer->group_flag & MULTIRES_SCULPT_LAYER_GROUP_HIDDEN) != 0;
        const bool group_collapsed =
            (layer->group_flag & MULTIRES_SCULPT_LAYER_GROUP_COLLAPSED) != 0;

        ui::Layout &group_row = layout.row(false);
        PointerRNA collapse_ptr = group_row.op(
            "SCULPT_OT_multires_layer_group_collapse",
            "",
            group_collapsed ? ICON_TRIA_RIGHT : ICON_TRIA_DOWN);
        RNA_int_set(&collapse_ptr, "group_id", layer->group_id);

        group_row.label(layer->group_name[0] ? layer->group_name : IFACE_("Group"),
                        ICON_FILE_FOLDER);

        PointerRNA visibility_ptr = group_row.op(
            "SCULPT_OT_multires_layer_group_visibility",
            group_hidden ? IFACE_("Show") : IFACE_("Hide"),
            group_hidden ? ICON_RESTRICT_VIEW_ON : ICON_RESTRICT_VIEW_OFF);
        RNA_int_set(&visibility_ptr, "group_id", layer->group_id);

        PointerRNA add_active_ptr = group_row.op(
            "SCULPT_OT_multires_layer_group_add_active",
            IFACE_("+ Active"),
            ICON_ADD);
        RNA_int_set(&add_active_ptr, "group_id", layer->group_id);

        PointerRNA merge_group_ptr = group_row.op(
            "SCULPT_OT_multires_layer_group_merge",
            IFACE_("Merge"),
            ICON_NONE);
        RNA_int_set(&merge_group_ptr, "group_id", layer->group_id);
      }

      last_sculpt_group_id = layer->group_id;
      if (layer->group_id != 0 &&
          (layer->group_flag & MULTIRES_SCULPT_LAYER_GROUP_COLLAPSED))
      {
        continue;
      }
      PointerRNA layer_ptr = RNA_pointer_create_discrete(
          &CTX_data_active_object(C)->id, RNA_MultiresSculptLayer, layer);
      ui::Layout &row = layout.row(false);
      PointerRNA op_ptr = row.op("SCULPT_OT_multires_layer_set_active",
                                 i == mmd->sculpt_layer_active ? IFACE_("Editing") : IFACE_("Select"),
                                 i == mmd->sculpt_layer_active ? ICON_RADIOBUT_ON :
                                                                  ICON_RADIOBUT_OFF);
      RNA_int_set(&op_ptr, "index", i);
      row.prop(&layer_ptr, "visible", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row.prop(&layer_ptr, "name", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      row.prop(&layer_ptr, "strength", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      PointerRNA solo_ptr = row.op("SCULPT_OT_multires_layer_solo",
                                   IFACE_("Solo"),
                                   ICON_RESTRICT_VIEW_OFF);
      RNA_int_set(&solo_ptr, "index", i);
    }
  }

  modifier_error_message_draw(layout, ptr);
}

static void subdivisions_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.enabled_set(RNA_enum_get(&ob_ptr, "mode") != OB_MODE_EDIT);

  MultiresModifierData *mmd = static_cast<MultiresModifierData *>(ptr->data);

  /**
   * Changing some of the properties can not be done once there is an
   * actual displacement stored for this multi-resolution modifier.
   * This check will disallow changes for those properties.
   * This check is a bit stupid but it should be sufficient for the usual
   * multi-resolution usage. It might become less strict and only disallow
   * modifications if there is CD_MDISPS layer, or if there is actual
   * non-zero displacement, but such checks will be too slow to be done
   * on every redraw.
   */

  PointerRNA op_ptr;
  op_ptr = layout.op("OBJECT_OT_multires_subdivide",
                     IFACE_("Subdivide"),
                     ICON_NONE,
                     wm::OpCallContext::ExecDefault,
                     UI_ITEM_NONE);
  RNA_enum_set(&op_ptr, "mode", int8_t(MultiresSubdivideModeType::CatmullClark));
  RNA_string_set(&op_ptr, "modifier", reinterpret_cast<ModifierData *>(mmd)->name);

  ui::Layout &row = layout.row(false);
  op_ptr = row.op("OBJECT_OT_multires_subdivide",
                  IFACE_("Simple"),
                  ICON_NONE,
                  wm::OpCallContext::ExecDefault,
                  UI_ITEM_NONE);
  RNA_enum_set(&op_ptr, "mode", int8_t(MultiresSubdivideModeType::Simple));
  RNA_string_set(&op_ptr, "modifier", reinterpret_cast<ModifierData *>(mmd)->name);
  op_ptr = row.op("OBJECT_OT_multires_subdivide",
                  IFACE_("Linear"),
                  ICON_NONE,
                  wm::OpCallContext::ExecDefault,
                  UI_ITEM_NONE);
  RNA_enum_set(&op_ptr, "mode", int8_t(MultiresSubdivideModeType::Linear));
  RNA_string_set(&op_ptr, "modifier", reinterpret_cast<ModifierData *>(mmd)->name);

  layout.separator();

  layout.op("OBJECT_OT_multires_unsubdivide", IFACE_("Unsubdivide"), ICON_NONE);
  layout.op("OBJECT_OT_multires_higher_levels_delete", IFACE_("Delete Higher"), ICON_NONE);
}

static void shape_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  const auto *mmd = static_cast<const MultiresModifierData *>(ptr->data);

  layout.enabled_set(RNA_enum_get(&ob_ptr, "mode") != OB_MODE_EDIT);

  PointerRNA op_ptr;
  ui::Layout *row = &layout.row(false);
  row->op("OBJECT_OT_multires_reshape", IFACE_("Reshape"), ICON_NONE);

  row = &layout.row(false);
  op_ptr = row->op("OBJECT_OT_multires_base_apply", IFACE_("Apply Base"), ICON_NONE);
  RNA_boolean_set(&op_ptr, "apply_heuristic", true);
  if (mmd->sculpt_layers.is_empty()) {
    op_ptr = row->op("OBJECT_OT_multires_base_apply", IFACE_("Conform Base"), ICON_NONE);
    RNA_boolean_set(&op_ptr, "apply_heuristic", false);
  }
  else {
    /* Both targets share the layer-preserving rebase. Keep their fitting choices explicit,
     * and route Base through that rebase instead of the layer-rejecting legacy operator. */
    ui::Layout &col = layout.column(false);
    col.enabled_set(RNA_enum_get(&ob_ptr, "mode") == OB_MODE_OBJECT);
    col.label(IFACE_("Conform — Preserve Layers (Experimental)"), ICON_NONE);
    op_ptr = col.op("OBJECT_OT_multires_conform_layers", IFACE_("Conform Base"), ICON_NONE);
    if (op_ptr.type != nullptr && op_ptr.data != nullptr) {
      RNA_boolean_set(&op_ptr, "include_visible_layers", false);
    }
    op_ptr = col.op(
        "OBJECT_OT_multires_conform_layers", IFACE_("Conform Layer-Aware"), ICON_NONE);
    if (op_ptr.type != nullptr && op_ptr.data != nullptr) {
      RNA_boolean_set(&op_ptr, "include_visible_layers", true);
    }
  }
}

static void generate_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  MultiresModifierData *mmd = static_cast<MultiresModifierData *>(ptr->data);

  bool is_external = RNA_boolean_get(ptr, "is_external");

  if (mmd->totlvl == 0) {
    layout.op("OBJECT_OT_multires_rebuild_subdiv", IFACE_("Rebuild Subdivisions"), ICON_NONE);
  }

  ui::Layout &col = layout.column(false);
  ui::Layout *row = &col.row(false);
  if (is_external) {
    row->op("OBJECT_OT_multires_external_pack", IFACE_("Pack External"), ICON_NONE);
    col.use_property_split_set(true);
    row = &col.row(false);
    row->prop(ptr, "filepath", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    col.op("OBJECT_OT_multires_external_save", IFACE_("Save External..."), ICON_NONE);
  }
}


static void displacement_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "use_sculpt_displacement", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &col = layout.column(false);
  col.active_set(RNA_boolean_get(ptr, "use_sculpt_displacement"));
  col.prop(ptr, "sculpt_displacement_texture", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "sculpt_displacement_coords", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const int mapping = RNA_enum_get(ptr, "sculpt_displacement_coords");
  if (mapping == 0) {
    col.prop(ptr, "sculpt_displacement_uv_layer", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (mapping == 3) {
    col.prop(ptr, "sculpt_displacement_coords_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "sculpt_displacement_coords_bone", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  col.prop(ptr, "sculpt_displacement_strength", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "sculpt_displacement_midlevel", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.op("SCULPT_OT_multires_texture_displacement_bake",
         IFACE_("Bake to Active Sculpt Layer"),
         ICON_NONE);
}

static void advanced_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  bool has_displacement = RNA_int_get(ptr, "total_levels") != 0;

  layout.use_property_split_set(true);

  layout.active_set(!has_displacement);

  layout.prop(ptr, "quality", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &col = layout.column(false);
  col.active_set(true);
  col.prop(ptr, "uv_smooth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "boundary_smooth", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.prop(ptr, "use_creases", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "use_custom_normals", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void shrinkwrap_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  const ui::eUI_Item_Flag toggles_flag = ui::ITEM_R_TOGGLE | ui::ITEM_R_FORCE_BLANK_DECORATE;
  layout.use_property_split_set(true);
  layout.prop(ptr, "use_sculpt_shrinkwrap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout &controls = layout.column(false);
  controls.active_set(RNA_boolean_get(ptr, "use_sculpt_shrinkwrap"));
  int wrap_method = RNA_enum_get(ptr, "sculpt_shrinkwrap_wrap_method");

  controls.prop(ptr, "sculpt_shrinkwrap_wrap_method", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  if (ELEM(wrap_method,
           MOD_SHRINKWRAP_PROJECT,
           MOD_SHRINKWRAP_NEAREST_SURFACE,
           MOD_SHRINKWRAP_TARGET_PROJECT))
  {
    controls.prop(ptr, "sculpt_shrinkwrap_wrap_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (wrap_method == MOD_SHRINKWRAP_PROJECT) {
    controls.prop(ptr, "sculpt_shrinkwrap_project_limit", UI_ITEM_NONE, IFACE_("Limit"), ICON_NONE);
    controls.prop(ptr, "sculpt_shrinkwrap_subsurf_levels", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    ui::Layout *col = &controls.column(false);
    ui::Layout &row = col->row(true, IFACE_("Axis"));
    row.prop(ptr, "sculpt_shrinkwrap_use_project_x", toggles_flag, std::nullopt, ICON_NONE);
    row.prop(ptr, "sculpt_shrinkwrap_use_project_y", toggles_flag, std::nullopt, ICON_NONE);
    row.prop(ptr, "sculpt_shrinkwrap_use_project_z", toggles_flag, std::nullopt, ICON_NONE);

    col->prop(ptr, "sculpt_shrinkwrap_use_negative_direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col->prop(ptr, "sculpt_shrinkwrap_use_positive_direction", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    controls.prop(ptr, "sculpt_shrinkwrap_cull_face", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    col = &controls.column(false);
    col->active_set(RNA_boolean_get(ptr, "sculpt_shrinkwrap_use_negative_direction") &&
                    RNA_enum_get(ptr, "sculpt_shrinkwrap_cull_face") != 0);
    col->prop(ptr, "sculpt_shrinkwrap_use_invert_cull", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  controls.prop(ptr, "sculpt_shrinkwrap_target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (wrap_method == MOD_SHRINKWRAP_PROJECT) {
    controls.prop(ptr, "sculpt_shrinkwrap_auxiliary_target", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  controls.prop(ptr, "sculpt_shrinkwrap_offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_vgroup_ui(controls, ptr, &ob_ptr, "sculpt_shrinkwrap_vertex_group", "sculpt_shrinkwrap_invert_vertex_group", std::nullopt);

  controls.prop(ptr, "sculpt_shrinkwrap_strength", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  controls.op("SCULPT_OT_multires_shrinkwrap_bake", IFACE_("Bake to Active Sculpt Layer"), ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Multires, panel_draw);
  modifier_subpanel_register(
      region_type, "subdivide", "Subdivision", nullptr, subdivisions_panel_draw, panel_type);
  modifier_subpanel_register(region_type, "shape", "Shape", nullptr, shape_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "generate", "Generate", nullptr, generate_panel_draw, panel_type);
  modifier_subpanel_register(region_type,
                             "texture_displacement",
                             "Texture Displacement",
                             nullptr,
                             displacement_panel_draw,
                             panel_type);
  modifier_subpanel_register(
      region_type, "advanced", "Advanced", nullptr, advanced_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "shrinkwrap", "Shrinkwrap", nullptr, shrinkwrap_panel_draw, panel_type);
}

ModifierTypeInfo modifierType_Multires = {
    /*idname*/ "Multires",
    /*name*/ N_("Multires"),
    /*struct_name*/ "MultiresModifierData",
    /*struct_size*/ sizeof(MultiresModifierData),
    /*srna*/ &RNA_MultiresModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
        eModifierTypeFlag_RequiresOriginalData,
    /*icon*/ ICON_MOD_MULTIRES,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ deform_matrices,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ free_data,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ depends_on_time,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ foreach_tex_link,
    /*free_runtime_data*/ free_runtime_data,
    /*panel_register*/ panel_register,
    /*blend_write*/ blend_write,
    /*blend_read*/ blend_read,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
