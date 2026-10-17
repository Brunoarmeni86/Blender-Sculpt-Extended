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
 * False means the caller must leave the stack and PBVH untouched.
 * Optional output certifies local top-level publication with an unchanged
 * live CCG surface; false is the conservative default for every other path. */
bool commit(Object &object, bool *r_local_surface_preserved = nullptr);

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
/** Whether any geometric layer, active/inactive Polypaint master or HR Face Set
 * owns companion data that topology operations must preserve. */
bool has_persistent_companions(const MultiresModifierData &mmd);
/** Keep a hidden per-corner identity attribute used to conservatively preserve
 * layer grids across Edit Mode topology changes. */
void ensure_corner_ids(MultiresModifierData &mmd, Mesh &mesh);
/** Bind newly initialized grids in current Mesh order. ONLY call immediately
 * after initializing fresh payload arrays, never to repair an unknown remap. */
bool bind_initialized_corner_grids(MultiresModifierData &mmd, const Mesh &mesh);
/** Remap layer grids from the hidden identity attribute after a topology edit.
 * Duplicated or invalid identities are treated as new corners with zero delta.
 * old_corner_ids is captured from the mesh BEFORE replacing it from BMesh and
 * describes the existing modifier grid order. Empty uses the legacy canonical order
 * only for verified legacy canonical IDs; otherwise the serialized modifier
 * binding supplies the old order. Explicit IDs must agree with an existing
 * binding. Non-canonical legacy files migrate via captured pre-Edit IDs.
 * Binding is copied/freed/serialized with the modifier, not the Mesh. */
bool remap_after_topology_edit(MultiresModifierData &mmd,
                               Mesh &mesh,
                               BitVector<> *r_reset_corners = nullptr,
                               Span<int> old_corner_ids = {});
/** Recording requires a visible layer with finite non-zero strength at a valid sculpt level, including L0. */
bool can_record(const MultiresModifierData &mmd, const Mesh &mesh);
/** Bake contributions into the native CD_MDISPS base. Reject hidden variants,
 * shared mesh data and non-finite results. Caller must commit first. */
bool bake_visible_to_base(MultiresModifierData &mmd, Mesh &mesh);
/** Explicit visible-only bake, same native cage and grid frame. Caller commits
 * pending Sculpt before entering Object Mode, establishes a pre-bake Memfile
 * checkpoint and tags mesh/object after publication. Hidden layers are retained.
 * Non-zero visible L0 deltas require frame rebasing and are rejected.
 * validate_only performs all finite/layout checks without writable access.
 * remove_layers=false retains empty layers with unchanged Strength/visibility.
 * Returns nullptr on success or an error before publishing any numeric data. */
const char *merge_visible_to_base(MultiresModifierData &mmd,
                                  Mesh &mesh,
                                  bool remove_layers = true,
                                  bool validate_only = false);

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

/** DETAIL_TRANSFER_V01: import point translations scattered using native
 * Multires vertex order into a staged new layer. Caller validates the frozen
 * preview topology/positions and establishes Object Mode Memfile Undo first.
 * Existing base/mask/color/Face Set/layer numeric payloads are never written. */
const char *detail_delta_import_to_new_layer(Object &object,
                                            const Mesh &delta_mesh,
                                            const char *name);

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
/** Merge visible layer contributions into the first layer; reject invalid input
 * without deleting or modifying source payloads. Keeps zero grids sparse. */
bool flatten(MultiresModifierData &mmd);
void set_solo(MultiresModifierData &mmd, int index);
void set_visible(MultiresSculptLayer &layer, bool visible);
void clear(MultiresModifierData &mmd);
void copy(MultiresModifierData &dst, const MultiresModifierData &src);

void polypaint_clear(MultiresModifierData &mmd);
/** Non-owning read view for one named attribute; never clear/free the view. */
bool polypaint_attribute_view(const MultiresModifierData &mmd,
                             const char *name, MultiresModifierData &r_view);
/** Main-thread ownership exchange, before brush Undo capture. No payload copy. */
bool polypaint_attribute_activate(MultiresModifierData &mmd, const char *name);
void polypaint_attribute_remove(MultiresModifierData &mmd, const char *name);
bool polypaint_attribute_rename(MultiresModifierData &mmd, const char *old_name,
                               const char *new_name);
void polypaint_attributes_clear(MultiresModifierData &mmd);
/** Opt-in independent Paint resolution. No geometry/layer publication. */
bool polypaint_lazy_master_enabled();
/** Stage a larger (or first) color master without modifying the source.
 * Only color-master fields in the empty destination are populated; caller owns
 * them through polypaint_clear. Existing samples are copied exactly, sparse
 * corners remain implicit. Never downsample or change base-corner topology. */
bool polypaint_stage_master(const MultiresModifierData &source,
                           int level,
                           int corners_num,
                           MultiresModifierData &destination);
/** Swap color-master ownership only; does not touch Sculpt Layers or geometry. */
void polypaint_swap_master(MultiresModifierData &a, MultiresModifierData &b);
float (*polypaint_grid_ensure(MultiresModifierData &mmd, int corner))[4];
const float (*polypaint_grid(const MultiresModifierData &mmd, int corner))[4];
bool polypaint_grid_discard(MultiresModifierData &mmd, int corner);
bool polypaint_resample(MultiresModifierData &mmd, int new_level);
bool polypaint_ensure(MultiresModifierData &mmd, int level, int grids_num);
/** Writer for the *current* sculpt resolution only. Never write master colors
 * through a lower-resolution CCG index. */
/* polypaint_grid_ensure(...) above is intentionally sculpt-level guarded. */
/** Explicit master-resolution writer for operations that know the stored stride
 * (for example, sculpt Undo). Does not change the color master resolution. */
float (*polypaint_grid_ensure_master(MultiresModifierData &mmd, int corner))[4];
/** Bilinear read-only sampling of high-resolution color at any sculpt level.
 * Grid u,v in [0,1]; false means absent/invalid and writes opaque white. */
bool polypaint_sample_grid(const MultiresColorGrid &grid, float u, float v, float r_rgba[4]);
bool polypaint_sample(const MultiresModifierData &mmd,
                      int corner,
                      float u,
                      float v,
                      float r_rgba[4]);


/** CPU-first Polypaint V0.2a: read existing painted master or interpolate the
 * named mesh Color Attribute (including BYTE_COLOR / POINT via attribute adaptation).
 * Returns false if neither valid painted data nor an attribute exists. */
bool polypaint_sample_with_base(const MultiresModifierData &mmd,
                                const Mesh &base_mesh,
                                const char *attribute_name,
                                int corner,
                                float u,
                                float v,
                                float r_rgba[4]);
/** Materialize a CCG grid only when painting first touches it. Initializes the
 * grid from the mesh attribute; never creates a silently-white source. */
float (*polypaint_grid_ensure_from_base(MultiresModifierData &mmd,
                                        const Mesh &base_mesh,
                                        const char *attribute_name,
                                        int corner))[4];
/** Paint a color sample at the requested sculpt level while keeping the
 * high-resolution master untouched except for an interpolated color difference.
 * Caller owns Undo transaction, redraw notification and neighboring seam copies. */
bool polypaint_write_coarse_sample(MultiresModifierData &mmd,
                                   const Mesh &base_mesh,
                                   const char *attribute_name,
                                   int corner,
                                   int paint_level,
                                   int x,
                                   int y,
                                   const float rgba[4]);

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

/* MULTIRES_FACESETS_DRAW_V01: categorical master editing, with serial preflight
 * before paint/Undo workers. Changes only Face Set IDs, never geometry. */
bool faceset_high_resolution_enabled();
bool faceset_master_valid(const MultiresModifierData &mmd, int grids_num);
int faceset_next_available_id(const MultiresModifierData &mmd, int base_next_id);
bool faceset_paint_grid(MultiresModifierData &mmd,
                       int corner,
                       int paint_side,
                       Span<float> factors,
                       float threshold,
                       int id);
bool faceset_grid_swap(MultiresModifierData &mmd, int corner, MutableSpan<int> undo_values);

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
