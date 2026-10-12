#include "BLI_bit_vector.hh"
/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_math_vector_types.hh"

/** \file
 * \ingroup bke
 */

namespace blender {

struct Depsgraph;
struct Mesh;
struct SubdivCCG;
struct Object;
struct Key;
struct KeyBlock;
struct MultiresModifierData;
struct MultiresSculptLayer;
struct MultiresColorGrid;
struct MultiresFaceSetGrid;

namespace bke::multires_layers {

/* Zero is reserved for new BMesh elements. V2 avoids interpreting legacy zero-based IDs as
 * the first surviving corner. Seed these attributes before converting the mesh to BMesh. */
inline constexpr char corner_id_attribute[] = ".sculpt_layer_corner_id_v2";
inline constexpr char corner_prev_attribute[] = ".sculpt_layer_corner_prev_v2";
inline constexpr char corner_next_attribute[] = ".sculpt_layer_corner_next_v2";
inline constexpr char corner_face_size_attribute[] = ".sculpt_layer_face_size_v2";
inline constexpr char vertex_id_attribute[] = ".sculpt_layer_vertex_id";

/** Runtime sparse-grid support index. Opt-in cache; output is an owned copy.
 * Invalidation accompanies payload allocation, free and existing layer edits. */
bool payload_grids(const MultiresSculptLayer &layer, int expected_grids, Vector<int> &r_grids);
void payload_index_invalidate(const MultiresSculptLayer *layer = nullptr);

/** True when a layer contributes to evaluation at all: visible, finite and non-zero strength.
 * This is useful for performance-sensitive callers which want to pre-filter layers before
 * iterating over every grid/sample. */
bool is_effective(const MultiresSculptLayer &layer);

/** L0 storage primitives. No original mesh positions are modified by these calls.
 * The sculpt/evaluation/undo integration is a separate gate. */
bool base_has_visible_layers(const MultiresModifierData &mmd);
MultiresModifierData *base_modifier(Object &object);
MultiresSculptLayer *base_recording_layer(Object &object);
void base_ensure_vertex_ids(Mesh &mesh);
bool base_remap_after_topology_edit(MultiresModifierData &mmd, Mesh &mesh);
void initialize_layer_grids(MultiresModifierData &mmd, int level, int corners_num);
bool base_deltas_ensure(MultiresSculptLayer &layer, int verts_num);
bool base_deltas_apply(const MultiresModifierData &mmd, MutableSpan<float3> positions);
bool base_deltas_record(MultiresSculptLayer &layer,
                        Span<int> vertices,
                        Span<float3> translations);
/** Explicit surviving-vertex map; -1 creates zero delta. All layers publish together. */
bool base_deltas_remap(MultiresModifierData &mmd, int old_verts_num, Span<int> new_to_old);

/** Commit pending CCG changes to the OLD active layer before changing the stack.
 * False means the caller must leave the stack and PBVH untouched. */
bool commit(Object &object);

/** Fast path for a relative Shape Key value change while Sculpt Multires is live.
 * Returns false when the caller must fall back to normal dependency-graph evaluation. */
bool shape_key_fast_update(Object &object, Key &key, KeyBlock &key_block);

/** Clear cached Shape Key support regions after structural Key edits. */
void shape_key_fast_cache_invalidate();
/** Validate every grid before modifying any displacement storage. */
bool compatible(const MultiresModifierData &mmd, const Mesh &mesh);
/** Resample all layer grids to a higher multires level without changing their values. */
bool resample_all(MultiresModifierData &mmd, int new_level, int grids_num);
/** Remap layer grids after a topology edit using an explicit surviving-corner map.
 * Each new corner maps to one old corner or -1 for newly created geometry.
 * Mapped corners must preserve their local grid orientation; callers must not infer
 * identity from the new corner index. Duplicate old indices are rejected.
 * Validate the whole stack before changing anything. New grids contain zero deltas.
 * This operates on layer data only; the caller owns native base MDisps migration.
 * Commit pending sculpt changes before calling and rebuild the evaluated mesh afterwards.
 */
bool remap_surviving_corners(MultiresModifierData &mmd,
                            int old_corners_num,
                            Span<int> new_to_old);
/** Keep a hidden per-corner identity attribute used to conservatively preserve
 * layer grids across Edit Mode topology changes. */
void ensure_corner_ids(MultiresModifierData &mmd, Mesh &mesh);
/** Remap layer grids from the hidden identity attribute after a topology edit.
 * Duplicated or invalid identities are treated as new corners with zero delta.
 * old_corner_ids is captured from the mesh BEFORE replacing it from BMesh and
 * describes the existing modifier grid order. Empty uses the legacy canonical order. */
bool remap_after_topology_edit(MultiresModifierData &mmd,
                               Mesh &mesh,
                               BitVector<> *r_reset_corners = nullptr,
                               Span<int> old_corner_ids = {});
/** Recording requires a visible layer with finite non-zero strength at a valid sculpt level, including L0. */
bool can_record(const MultiresModifierData &mmd, const Mesh &mesh);
/** Bake contributions into the native CD_MDISPS base. Reject hidden variants,
 * shared mesh data and non-finite results. Caller must commit first. */
bool bake_visible_to_base(MultiresModifierData &mmd, Mesh &mesh);
/** Bake procedural Multires texture displacement into the active Sculpt Layer.
 * When respect_mask is true, only unmasked influence is baked and live displacement is left enabled
 * by the operator so masked regions keep their appearance. */
bool bake_texture_displacement_to_active(Depsgraph *depsgraph,
                                         Object &object,
                                         bool respect_mask,
                                         bool &r_used_mask);
bool shrinkwrap_mesh(Depsgraph *depsgraph,
                     Object &object,
                     const MultiresModifierData &mmd,
                     Mesh &mesh,
                     bool respect_mask = true,
                     Span<float> sampled_masks = {});
bool shrinkwrap_ccg(Depsgraph *depsgraph,
                    Object &object,
                    const MultiresModifierData &mmd,
                    const Mesh &base_mesh,
                    SubdivCCG &ccg,
                    bool respect_mask = true);
bool bake_shrinkwrap_to_active(Depsgraph *depsgraph,
                              Object &object,
                              bool respect_mask,
                              bool &r_used_mask);

MultiresSculptLayer *add(MultiresModifierData &mmd, const char *name, int level, int grids_num);
void remove(MultiresModifierData &mmd, MultiresSculptLayer &layer);
MultiresSculptLayer *active(MultiresModifierData &mmd);
const MultiresSculptLayer *active(const MultiresModifierData &mmd);
void set_active(MultiresModifierData &mmd, int index);
/** Reorder a layer while keeping the same logical layer active.
 * Callers must commit pending sculpt changes before changing stack order. */
bool move(MultiresModifierData &mmd, int from_index, int to_index);
/* Lightweight organizational Sculpt Layer groups. */
bool group_with_below(MultiresModifierData &mmd);
bool group_create_from_active(MultiresModifierData &mmd);
bool group_add_active(MultiresModifierData &mmd, int group_id);
bool group_merge(MultiresModifierData &mmd, int group_id);
bool group_remove_active(MultiresModifierData &mmd);
bool group_toggle_visibility(MultiresModifierData &mmd, int group_id);
bool group_toggle_collapsed(MultiresModifierData &mmd, int group_id);

MultiresSculptLayer *duplicate(MultiresModifierData &mmd, const MultiresSculptLayer &source);
MultiresSculptLayer *previous(MultiresModifierData &mmd, const MultiresSculptLayer &layer);
bool merge_down(MultiresModifierData &mmd);
void flatten(MultiresModifierData &mmd);
void set_solo(MultiresModifierData &mmd, int index);
void set_visible(MultiresSculptLayer &layer, bool visible);
void clear(MultiresModifierData &mmd);
void copy(MultiresModifierData &dst, const MultiresModifierData &src);

void polypaint_clear(MultiresModifierData &mmd);
float (*polypaint_grid_ensure(MultiresModifierData &mmd, int corner))[4];
const float (*polypaint_grid(const MultiresModifierData &mmd, int corner))[4];
bool polypaint_grid_discard(MultiresModifierData &mmd, int corner);
bool polypaint_resample(MultiresModifierData &mmd, int new_level);
bool polypaint_ensure(MultiresModifierData &mmd, int level, int grids_num);

/** Persistent high-resolution Face Sets on Multires CCG grids.
 * Uniform grids stay allocation-free. Dense IDs are created lazily only for
 * grids containing a boundary. The stored level is a master level: sampling a
 * lower sculpt level never destroys high-resolution data. */
void faceset_clear(MultiresModifierData &mmd);
bool faceset_ensure_from_mesh(MultiresModifierData &mmd, const Mesh &mesh, int level);
bool faceset_resample_up(MultiresModifierData &mmd, int new_level);
int *faceset_grid_ensure_dense(MultiresModifierData &mmd, int corner);
const int *faceset_grid(const MultiresModifierData &mmd, int corner);
int faceset_sample(const MultiresModifierData &mmd,
                   int corner,
                   float grid_u,
                   float grid_v,
                   int fallback_id);

/** Add the layer displacement to a tangent-space displacement vector.
 * Returns false when the layer cannot be evaluated at the requested level.
 */
/* V4.5a sparse Sculpt Layer foundation.
 * A null per-grid displacement payload represents an implicit all-zero grid.
 * grid_ensure() materializes only the requested grid. */
float (*grid_ensure(MultiresSculptLayer &layer, int corner))[3];
bool sample_tangent(const MultiresSculptLayer &layer,
                    int corner,
                    int grid_size,
                    float grid_u,
                    float grid_v,
                    float r_tangent_D[3]);


/* LAYER_VISIBLE_DIRECT_DELTA_V2_1_BKE */
/* LAYER_WEIGHT_HOT_CACHE_INVALIDATION_V1
 * Drop cached object-space layer deltas after persistent payload mutation.
 * nullptr invalidates the current hot layer unconditionally. */
void weight_hot_delta_cache_invalidate(const MultiresSculptLayer *layer = nullptr);

bool weight_direct_delta(Object &object,
                         MultiresModifierData &mmd,
                         const MultiresSculptLayer &layer,
                         float weight_delta,
                         blender::BitVector<> &r_affected_faces,
                         bool *r_hot_cache_hit = nullptr);

bool visible_direct_delta(Object &object,
                          MultiresModifierData &mmd,
                          const MultiresSculptLayer &layer,
                          bool make_visible,
                          blender::BitVector<> &r_affected_faces);

}  // namespace bke::multires_layers
}  // namespace blender



