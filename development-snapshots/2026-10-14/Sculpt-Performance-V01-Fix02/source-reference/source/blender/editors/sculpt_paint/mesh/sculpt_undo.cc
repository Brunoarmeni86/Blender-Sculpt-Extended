/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode undo system.
 *
 * Usage Guide
 * ===========
 *
 * The sculpt undo system is a delta-based system. Each undo step stores the difference with the
 * prior one.
 *
 * To use the sculpt undo system, you must call #push_begin inside an operator exec or invoke
 * callback (#geometry_begin may be called if you wish to save a non-delta copy of the entire
 * mesh). This will initialize the sculpt undo stack and set up an undo step.
 *
 * At the end of the operator you should call #push_end.
 *
 * #push_begin and #geometry_begin both take a #wmOperatorType as an argument. There are _ex
 * versions that allow a custom name; try to avoid using them. These can break the redo panel since
 * it requires the undo push to have the same name as the calling operator.
 */
#include "sculpt_undo.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#include <mutex>
#include <zstd.h>

#include "CLG_log.h"

#include "BLI_array.hh"
#include "BLI_bit_group_vector.hh"
#include "BLI_compression.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_memory_counter.hh"
#include "BLI_memory_utils.hh"
#include "BLI_time.h"
#include "BLI_string_utf8.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_key_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_scene.hh"
#include "BKE_subdiv.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_undo_system.hh"

/* TODO(sergey): Ideally should be no direct call to such low level things. */
#include "BKE_subdiv_eval.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_geometry.hh"
#include "ED_object.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"

#include "bmesh.hh"
#include "mesh_brush_common.hh"
#include "paint_hide.hh"
#include "sculpt_color.hh"
#include "sculpt_dyntopo.hh"
#include "sculpt_face_set.hh"
#include "sculpt_intern.hh"

namespace blender {

// #define DEBUG_TIME

#ifdef DEBUG_TIME
#  include "BLI_timeit.hh"
#endif

static CLG_LogRef LOG = {"undo.sculpt"};

namespace ed::sculpt_paint::undo {

/* Implementation of undo system for objects in sculpt mode.
 *
 * Each undo step in sculpt mode consists of list of nodes, each node contains a flat array of data
 * related to the step type.
 *
 * Node type used for undo depends on specific operation and active sculpt mode ("regular" or
 * dynamic topology).
 *
 * Regular sculpt brushes will use Position, HideVert, HideFace, Mask, FaceSet nodes. These
 * nodes are created for every BVH node which is affected by the brush. The undo push for the node
 * happens BEFORE modifications. This makes the operation undo to work in the following way: for
 * every node in the undo step swap happens between node in the undo stack and the corresponding
 * value in the BVH. This is how redo is possible after undo.
 *
 * The COORDS, HIDDEN or MASK type of nodes contains arrays of the corresponding values.
 *
 * Operations like Symmetrize are using GEOMETRY type of nodes which pushes the entire state of the
 * mesh to the undo stack. This node contains all CustomData layers.
 *
 * The tricky aspect of this undo node type is that it stores mesh before and after modification.
 * This allows the undo system to both undo and redo the symmetrize operation within the
 * pre-modified-push of other node type behavior, but it uses more memory that it seems it should
 * be.
 *
 * The dynamic topology undo nodes are handled somewhat separately from all other ones and the idea
 * there is to store log of operations: which vertices and faces have been added or removed.
 *
 * Begin of dynamic topology sculpting mode have own node type. It contains an entire copy of mesh
 * since just enabling the dynamic topology mode already does modifications on it.
 *
 * End of dynamic topology and symmetrize in this mode are handled in a special manner as well. */

#define NO_ACTIVE_LAYER bke::AttrDomain::Auto

struct Node {
  Array<float3, 0> position;
  Array<float3, 0> orig_position;
  Array<float3, 0> normal;
  Array<float4, 0> col;
  /* Persisted COLOR master CCG side, independent of current Sculpt Level. */
  int grid_color_side = 0;
  /* Track implicit (base attribute) vs actual master colors for Undo. */
  BitVector<0> grid_color_present;
  Array<float, 0> mask;

  Array<float4, 0> loop_col;

  /* Mesh. */

  Array<int, 0> vert_indices;
  int unique_verts_num;

  /**
   * \todo Storing corners rather than faces is unnecessary.
   */
  Vector<int, 0> corner_indices;

  BitVector<0> vert_hidden;
  BitVector<0> face_hidden;

  /* Multires. */

  /** Indices of grids in the pbvh::Tree node. */
  Array<int, 0> grids;
  BitGroupVector<0> grid_hidden;

  /* SCULPT_LAYER_UNDO_PERSISTENT_V3.
   * Pre-stroke persistent, unweighted displacement of the active Sculpt Layer for these grids.
   * The layer can be stored at the Multires top level even when the live CCG is lower level. */
  Array<float3, 0> sculpt_layer_disps;
  int sculpt_layer_grid_area = 0;

  /* Sculpt Face Sets */
  Array<int, 0> face_sets;

  /* High-resolution Multires Face Set master values for this undo node's grids. */
  Array<int, 0> grid_face_sets;
  int grid_face_set_side = 0;

  Vector<int> face_indices;
};

struct SculptAttrRef {
  bke::AttrDomain domain;
  eCustomDataType type;
  char name[MAX_CUSTOMDATA_LAYER_NAME];
  bool was_set;
};

/* Storage of geometry for the undo node.
 * Is used as a storage for either original or modified geometry. */
struct NodeGeometry {
  /* Is used for sanity check, helping with ensuring that two and only two
   * geometry pushes happened in the undo stack. */
  bool is_initialized;

  bke::AttributeStorage attribute_storage;
  CustomData vert_data;
  CustomData edge_data;
  CustomData corner_data;
  CustomData face_data;
  int *face_offset_indices;
  const ImplicitSharingInfo *face_offsets_sharing_info;
  int verts_num;
  int edges_num;
  int corners_num;
  int faces_num;
};

struct Node;
struct PositionUndoStorage;

/* SCULPT_LAYER_UNDO_METADATA_V1 */
struct SculptLayerUndoMetadata {
  std::string name;
  float strength;
  bool visible;
};

static bool sculpt_layer_undo_metadata_enabled()
{
  const char *value = std::getenv("BLENDER_SCULPT_LAYER_UNDO_METADATA");
  return value && value[0] == '1';
}


/* SCULPT_LAYER_UNDO_FRAME_V1: store composition metadata, never high-res payloads. */
struct SculptLayerUndoFrameItem {
  std::string name;
  float strength;
  bool visible;
};

static bool sculpt_layer_undo_frame_enabled()
{
  const char *value = std::getenv("BLENDER_SCULPT_LAYER_UNDO_FRAME");
  return value && value[0] == '1';
}


/* CCG coordinates are the sole stroke undo payload. Persistent deltas are written by the
 * normal pending flush; the captured composition frame handles weight/level transitions.
 * Set BLENDER_SCULPT_LAYER_UNDO_CCG=0 to compare the previous persistent implementation. */
static bool sculpt_layer_undo_ccg_enabled()
{
  const char *value = std::getenv("BLENDER_SCULPT_LAYER_UNDO_CCG");
  return value == nullptr || value[0] == '\0' || value[0] != '0';
}

/* SCULPT_LAYER_UNDO_PERSISTENT_V3
 *
 * Position undo for Sculpt Layers must exchange the persistent, unweighted layer payload,
 * not only the already-composed CCG surface. Enabled by default for this fork; set
 * BLENDER_SCULPT_LAYER_UNDO_PERSISTENT_V3=0 for emergency A/B fallback. */
static bool sculpt_layer_undo_persistent_enabled()
{
  const char *value = std::getenv("BLENDER_SCULPT_LAYER_UNDO_PERSISTENT_V3");
  return value == nullptr || value[0] == '\0' || value[0] != '0';
}


struct StepData {
 private:
  bool applied_ = true;

 public:
  /**
   * The type of data stored in this undo step. For historical reasons this is often set when the
   * first undo node is pushed.
   */
  Type type = Type::None;

  /* Lightweight post-Add state replacing the visible Memfile history entry.
   * The preceding Memfile is retained, not decoded for the first stroke undo. */
  bool layer_add_boundary_anchor = false;

  /** Name of the object associated with this undo data (`object.id.name`). */
  std::string object_name;

  /** Name of the object's active shape key when the undo step was created. */
  std::string active_shape_key_name;
  std::string base_modifier_name;
  std::string base_layer_name;
  Array<float3> base_layer_deltas;
  std::string layer_frame_modifier;
  Vector<SculptLayerUndoFrameItem> layer_frame;
  int layer_frame_active = -1;
  int layer_frame_level = 0;
  int layer_frame_top_level = 0;
  /* Per-step decision made before the stroke: never pair pending CCG coordinates with
   * stale persistent deltas, or a lower-level surface with a partial top-level snapshot. */
  bool layer_persistent_snapshot_valid = false;
  /* A refresh restores persistent data without exchanging compressed_positions. Once that
   * happens, those coordinates must no longer be used as the paired redo representation. */
  bool layer_position_buffers_match_payload = true;
  /* The stroke edits native base MDisps while every stored layer is hidden. */
  bool native_multires_base_undo = false;
  std::string layer_metadata_modifier;
  Vector<SculptLayerUndoMetadata> layer_metadata;


  /* TODO: Combine the three structs into a variant, since they specify data that is only valid
   * within a single mode. */
  struct {
    /* The number of vertices in the entire mesh. */
    int verts_num;
    /* The number of face corners in the entire mesh. */
    int corners_num;
  } mesh;

  struct {
    /** The number of grids in the entire mesh. A copy of #SubdivCCG::grids_num. */
    int grids_num;
    /** A copy of #SubdivCCG::grid_size. */
    int grid_size;
  } grids;

  struct {
    /**
     * The current log entry for the given BMLog step. Represents the most recent step at the time
     * that this entry is added.
     *
     * There are two usages of this pointer:
     * - If undoing or redoing a enter / exit from Dyntopo, this entry is used to rebuild the
     *   BMLog from all of the relevant entries
     * - When an undo step is no longer valid, this is used to free the data that it holds and
     *   remove it from the underlying list.
     */
    BMLogEntry *bm_entry;

    /* Geometry at the bmesh enter moment. */
    NodeGeometry geometry_enter;
  } bmesh;

  float3 pivot_pos;
  float4 pivot_rot;

  /* Geometry modification operations. */
  /* Original geometry is stored before the modification and is restored from when undoing. */
  NodeGeometry geometry_original;
  /* Modified geometry is stored after the modification and is restored from when redoing. */
  NodeGeometry geometry_modified;

  Mutex nodes_mutex;

  /**
   * #undo::Node is stored per #pbvh::Node to reduce data storage needed for changes only impacting
   * small portions of the mesh. During undo step creation and brush evaluation we often need to
   * look up the undo state for a specific node. That lookup must be protected by a lock since
   * nodes are pushed from multiple threads. This map speeds up undo node access to reduce the
   * amount of time we wait for the lock.
   *
   * This is only accessible when building the undo step, in between #push_begin and #push_end.
   */
  Map<const bke::pbvh::Node *, std::unique_ptr<Node>> undo_nodes_by_pbvh_node;

  /** Storage of per-node undo data after creation of the undo step is finished. */
  Vector<std::unique_ptr<Node>> nodes;
  std::unique_ptr<PositionUndoStorage> position_step_storage;
  size_t undo_size;

  /** Whether processing code needs to handle the current data as an undo step. */
  bool needs_undo() const
  {
    return applied_;
  }

  void tag_needs_undo()
  {
    applied_ = true;
  }

  void tag_needs_redo()
  {
    applied_ = false;
  }
};

namespace compression {

/**
 * Compress a span, using a prefiltering step that can improve compression speed and ratios for
 * certain float data types.
 */
template<typename T>
void filter_compress(const Span<T> src,
                     Vector<std::byte> &filter_buffer,
                     Vector<std::byte> &compress_buffer)
{
  PRF_scope(ProfileCategory::Editor);
  filter_buffer.resize(src.size_in_bytes());
  filter_transpose_delta(reinterpret_cast<const uint8_t *>(src.data()),
                         reinterpret_cast<uint8_t *>(filter_buffer.data()),
                         src.size(),
                         sizeof(T));

  /* Level 3 gives a good balance of compression performance and ratio, and is also used elsewhere
   * across Blender for calls to #ZSTD_compress. */
  constexpr int zstd_level = 3;
  compress_buffer.resize(ZSTD_compressBound(src.size_in_bytes()));
  const size_t dst_size = ZSTD_compress(compress_buffer.data(),
                                        compress_buffer.size(),
                                        filter_buffer.data(),
                                        filter_buffer.size(),
                                        zstd_level);
  if (ZSTD_isError(dst_size)) {
    compress_buffer.clear();
    return;
  }

  compress_buffer.resize(dst_size);
}

template<typename T>
void filter_decompress(const Span<std::byte> src, Vector<std::byte> &buffer, Vector<T> &dst)
{
  PRF_scope(ProfileCategory::Editor);
  const unsigned long long dst_size_in_bytes = ZSTD_getFrameContentSize(src.data(), src.size());
  if (ELEM(dst_size_in_bytes, ZSTD_CONTENTSIZE_ERROR, ZSTD_CONTENTSIZE_UNKNOWN)) {
    dst.clear();
    return;
  }

  buffer.resize(dst_size_in_bytes);
  const size_t result = ZSTD_decompress(buffer.data(), buffer.size(), src.data(), src.size());
  if (ZSTD_isError(result)) {
    dst.clear();
    return;
  }

  dst.resize(buffer.size() / sizeof(T));
  unfilter_transpose_delta(reinterpret_cast<const uint8_t *>(buffer.data()),
                           reinterpret_cast<uint8_t *>(dst.data()),
                           dst.size(),
                           sizeof(T));
}

template void filter_compress<float3>(Span<float3>, Vector<std::byte> &, Vector<std::byte> &);
template void filter_compress<int>(Span<int>, Vector<std::byte> &, Vector<std::byte> &);

template void filter_decompress<float3>(Span<std::byte>, Vector<std::byte> &, Vector<float3> &);
template void filter_decompress<int>(Span<std::byte>, Vector<std::byte> &, Vector<int> &);

}  // namespace compression

struct PositionUndoStorage : NonMovable {
  Vector<std::unique_ptr<Node>> nodes_to_compress;
  bool multires_undo;

  Array<Array<std::byte>> compressed_indices;

  /* As undo and redo happen, the data in these arrays is swapped (an undo step becomes a redo
   * step, and vice versa). */
  Array<Array<std::byte>> compressed_positions;

  /* Persistent Sculpt Layer payload paired with compressed_indices. Unlike compressed_positions,
   * this is always the unweighted top-level layer delta, independent of Strength. */
  Array<Array<std::byte>> compressed_layer_disps;
  bool sculpt_layer_undo = false;
  int sculpt_layer_grid_area = 0;

  Array<int> unique_verts_nums;

  TaskPool *compression_task_pool;
  std::atomic<bool> compression_ready = false;
  std::atomic<bool> compression_started = false;
  StepData *owner_step_data = nullptr;

  explicit PositionUndoStorage(StepData &step_data)
      : nodes_to_compress(std::move(step_data.nodes)), owner_step_data(&step_data)
  {
    this->multires_undo = step_data.grids.grids_num != 0;
    if (!multires_undo) {
      this->unique_verts_nums.reinitialize(this->nodes_to_compress.size());
      for (const int i : this->nodes_to_compress.index_range()) {
        this->unique_verts_nums[i] = this->nodes_to_compress[i]->unique_verts_num;
      }
    }
    else if (!this->nodes_to_compress.is_empty()) {
      const int area = this->nodes_to_compress[0]->sculpt_layer_grid_area;
      bool valid_layer_payload = area > 0;
      for (const std::unique_ptr<Node> &node : this->nodes_to_compress) {
        valid_layer_payload &= node->sculpt_layer_grid_area == area &&
                               node->sculpt_layer_disps.size() ==
                                   int64_t(node->grids.size()) * int64_t(area);
      }
      if (valid_layer_payload) {
        this->sculpt_layer_undo = true;
        this->sculpt_layer_grid_area = area;
      }
    }

    this->compression_task_pool = BLI_task_pool_create_background(this, TASK_PRIORITY_LOW);
    this->compression_started = true;

    BLI_task_pool_push(this->compression_task_pool, compress_fn, this, false, nullptr);
  }

  ~PositionUndoStorage()
  {
    if (compression_started.load() && compression_task_pool) {
      BLI_task_pool_work_and_wait(compression_task_pool);
      BLI_task_pool_free(compression_task_pool);
    }
  }

  void ensure_compression_complete()
  {
    if (!compression_ready.load(std::memory_order_acquire)) {
      const char *profile = std::getenv("BLENDER_SCULPT_UNDO_COMPRESSION_PROFILE");
      const bool trace = profile && profile[0] == '1' && profile[1] == '\0';
      const double started = trace ? BLI_time_now_seconds() : 0.0;
      BLI_task_pool_work_and_wait(compression_task_pool);
      if (trace) {
        std::printf("[UNDO_COMPRESSION_WAIT] elapsed=%.3fms ready=%d\n",
                    (BLI_time_now_seconds() - started) * 1000.0,
                    int(compression_ready.load(std::memory_order_acquire)));
      }
    }
  }

  static void compress_fn(TaskPool * /*pool*/, void *task_data)
  {
#ifdef DEBUG_TIME
    SCOPED_TIMER_AVERAGED(__func__);
#endif
    auto *data = static_cast<PositionUndoStorage *>(task_data);
    MutableSpan<std::unique_ptr<Node>> nodes = data->nodes_to_compress;
    const int nodes_num = nodes.size();

    Array<Array<std::byte>> compressed_indices(nodes.size(), NoInitialization());
    Array<Array<std::byte>> compressed_data(nodes.size(), NoInitialization());
    Array<Array<std::byte>> compressed_layer_data(
        data->sculpt_layer_undo ? nodes.size() : 0, NoInitialization());
    struct CompressLocalData {
      Vector<std::byte> filtered;
      Vector<std::byte> compressed;
    };
    threading::isolate_task([&]() {
      threading::EnumerableThreadSpecific<CompressLocalData> all_tls;
      threading::parallel_for(IndexRange(nodes_num), 1, [&](const IndexRange range) {
        CompressLocalData &local_data = all_tls.local();
        for (const int i : range) {
          const Span<int> indices = data->multires_undo ? nodes[i]->grids : nodes[i]->vert_indices;
          const Span<float3> positions = !nodes[i]->orig_position.is_empty() ?
                                             nodes[i]->orig_position :
                                             nodes[i]->position;
          compression::filter_compress(indices, local_data.filtered, local_data.compressed);
          new (&compressed_indices[i]) Array<std::byte>(local_data.compressed.as_span());
          compression::filter_compress(positions, local_data.filtered, local_data.compressed);
          new (&compressed_data[i]) Array<std::byte>(local_data.compressed.as_span());
          if (data->sculpt_layer_undo) {
            compression::filter_compress(
                nodes[i]->sculpt_layer_disps.as_span(), local_data.filtered, local_data.compressed);
            new (&compressed_layer_data[i]) Array<std::byte>(local_data.compressed.as_span());
          }
          nodes[i].reset();
        }
      });
    });
    data->nodes_to_compress.clear_and_shrink();

    size_t memory_size = 0;
    for (const int i : IndexRange(nodes_num)) {
      memory_size += compressed_indices[i].as_span().size_in_bytes();
      memory_size += compressed_data[i].as_span().size_in_bytes();
      if (data->sculpt_layer_undo) {
        memory_size += compressed_layer_data[i].as_span().size_in_bytes();
      }
    }

    data->compressed_indices = std::move(compressed_indices);
    data->compressed_positions = std::move(compressed_data);
    if (data->sculpt_layer_undo) {
      data->compressed_layer_disps = std::move(compressed_layer_data);
    }
    data->owner_step_data->undo_size += memory_size;

    data->compression_ready.store(true, std::memory_order_release);
  }
};

struct SculptUndoStep {
  UndoStep step;
  /* NOTE: will split out into list for multi-object-sculpt-mode. */
  StepData data;

  /* Active color attribute at the start of this undo step. */
  SculptAttrRef active_color_start;

  /* Active color attribute at the end of this undo step. */
  SculptAttrRef active_color_end;
};

size_t step_memory_size_get(UndoStep *step)
{
  if (step->type != BKE_UNDOSYS_TYPE_SCULPT) {
    return 0;
  }

  SculptUndoStep *sculpt_step = reinterpret_cast<SculptUndoStep *>(step);

  if (sculpt_step->data.position_step_storage) {
    sculpt_step->data.position_step_storage->ensure_compression_complete();
  }

  return sculpt_step->data.undo_size;
}

static SculptUndoStep *get_active_step()
{
  UndoStack *ustack = ED_undo_stack_get();
  UndoStep *us = BKE_undosys_stack_init_or_active_with_type(ustack, BKE_UNDOSYS_TYPE_SCULPT);
  return reinterpret_cast<SculptUndoStep *>(us);
}

static StepData *get_step_data()
{
  if (SculptUndoStep *us = get_active_step()) {
    return &us->data;
  }
  return nullptr;
}

static bool use_multires_undo(const StepData &step_data, const SculptSession &ss)
{
  return step_data.grids.grids_num != 0 && ss.subdiv_ccg != nullptr;
}

static bool topology_matches(const StepData &step_data, const Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  if (use_multires_undo(step_data, ss)) {
    const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
    return subdiv_ccg.grids_num == step_data.grids.grids_num &&
           subdiv_ccg.grid_size == step_data.grids.grid_size;
  }
  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  return mesh.verts_num == step_data.mesh.verts_num;
}

static bool indices_contain_true(const Span<bool> data, const Span<int> indices)
{
  return std::any_of(indices.begin(), indices.end(), [&](const int i) { return data[i]; });
}

static bool restore_active_shape_key(bContext &C,
                                     Depsgraph &depsgraph,
                                     const StepData &step_data,
                                     Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  if (ss.shapekey_active && ss.shapekey_active->name != step_data.active_shape_key_name) {
    /* Shape key has been changed before calling undo operator. */

    Key *key = BKE_key_from_object(&object);
    const KeyBlock *kb = key ?
                             BKE_keyblock_find_name(key, step_data.active_shape_key_name.c_str()) :
                             nullptr;

    if (kb) {
      object.shapenr = BLI_findindex(&key->block, kb) + 1;

      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      WM_event_add_notifier(&C, NC_OBJECT | ND_DATA, &object);
    }
    else {
      /* Key has been removed -- skip this undo node. */
      return false;
    }
  }
  return true;
}

template<typename T>
static void swap_indexed_data(MutableSpan<T> full, const Span<int> indices, MutableSpan<T> indexed)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(full.size() == indices.size());
  for (const int i : indices.index_range()) {
    std::swap(full[i], indexed[indices[i]]);
  }
}

static void restore_position_mesh(Object &object,
                                  PositionUndoStorage &undo_data,
                                  const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  std::optional<ShapeKeyData> shape_key_data = ShapeKeyData::from_object(object);

  undo_data.ensure_compression_complete();

  const int nodes_num = undo_data.unique_verts_nums.size();

  struct LocalData {
    Vector<std::byte> compress_buffer;
    Vector<std::byte> filter_buffer;
    Vector<int> indices;
    Vector<float3> positions;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  threading::parallel_for(IndexRange(nodes_num), 1, [&](const IndexRange range) {
    LocalData &tls = all_tls.local();
    for (const int i : range) {
      compression::filter_decompress<int>(
          undo_data.compressed_indices[i], tls.compress_buffer, tls.indices);
      const int unique_verts_num = undo_data.unique_verts_nums[i];
      const Span<int> verts = tls.indices.as_span().take_front(unique_verts_num);

      compression::filter_decompress<float3>(
          undo_data.compressed_positions[i], tls.compress_buffer, tls.positions);
      MutableSpan undo_positions = tls.positions.as_mutable_span();

      if (!ss.deform_modifiers_active && !shape_key_data) {
        /* When original positions aren't written separately in the undo step, there are no
         * deform modifiers. Therefore the original and evaluated deform positions will be the
         * same, and modifying the positions from the original mesh is enough. */
        swap_indexed_data(undo_positions.take_front(unique_verts_num), verts, positions);
      }
      else {
        /* When original positions are stored in the undo step, undo/redo will cause a reevaluation
         * of the object. The evaluation will recompute the evaluated positions, so dealing with
         * them here is unnecessary. */
        if (shape_key_data) {
          MutableSpan<float3> active_data = shape_key_data->active_key_data;

          if (!shape_key_data->dependent_keys.is_empty()) {
            Array<float3, 1024> translations(verts.size());
            translations_from_new_positions(
                undo_positions.take_front(unique_verts_num), verts, active_data, translations);
            for (MutableSpan<float3> data : shape_key_data->dependent_keys) {
              apply_translations(translations, verts, data);
            }
          }

          if (shape_key_data->basis_key_active) {
            /* The basis key positions and the mesh positions are always kept in sync. */
            scatter_data_mesh(undo_positions.as_span(), verts, positions);
          }
          swap_indexed_data(undo_positions.take_front(unique_verts_num), verts, active_data);
        }
        else {
          /* There is a deform modifier, but no shape keys. */
          swap_indexed_data(undo_positions.take_front(unique_verts_num), verts, positions);
        }
      }

      modified_verts.fill_indices(verts, true);

      compression::filter_compress<float3>(undo_positions, tls.filter_buffer, tls.compress_buffer);
      undo_data.compressed_positions[i] = tls.compress_buffer.as_span();
    }
  });
}

static void restore_position_grids(const MutableSpan<float3> positions,
                                   const CCGKey &key,
                                   PositionUndoStorage &undo_data,
                                   const MutableSpan<bool> modified_grids)
{
  PRF_scope(ProfileCategory::Editor);
  /* Compression publishes indices and positions asynchronously. Wait before reading either
   * array, including its size, just as restore_position_mesh does. */
  undo_data.ensure_compression_complete();
  const int nodes_num = undo_data.compressed_indices.size();

  struct LocalData {
    Vector<std::byte> compress_buffer;
    Vector<std::byte> filter_buffer;
    Vector<int> indices;
    Vector<float3> positions;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  threading::parallel_for(IndexRange(nodes_num), 1, [&](const IndexRange range) {
    LocalData &tls = all_tls.local();
    for (const int i : range) {
      compression::filter_decompress<int>(
          undo_data.compressed_indices[i], tls.compress_buffer, tls.indices);
      const Span<int> grids = tls.indices.as_span();

      compression::filter_decompress<float3>(
          undo_data.compressed_positions[i], tls.compress_buffer, tls.positions);
      MutableSpan node_positions = tls.positions.as_mutable_span();

      for (const int i : grids.index_range()) {
        MutableSpan data = positions.slice(bke::ccg::grid_range(key, grids[i]));
        MutableSpan undo_data = node_positions.slice(bke::ccg::grid_range(key, i));
        for (const int offset : data.index_range()) {
          std::swap(data[offset], undo_data[offset]);
        }
      }

      modified_grids.fill_indices(grids, true);

      compression::filter_compress<float3>(node_positions, tls.filter_buffer, tls.compress_buffer);
      undo_data.compressed_positions[i] = tls.compress_buffer.as_span();
    }
  });
}

static void restore_vert_visibility_mesh(Object &object,
                                         Node &unode,
                                         const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_vert = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);
  for (const int i : unode.vert_indices.index_range().take_front(unode.unique_verts_num)) {
    const int vert = unode.vert_indices[i];
    if (unode.vert_hidden[i].test() != hide_vert.span[vert]) {
      unode.vert_hidden[i].set(!unode.vert_hidden[i].test());
      hide_vert.span[vert] = !hide_vert.span[vert];
      modified_verts[vert] = true;
    }
  }
  hide_vert.finish();
}

static void restore_vert_visibility_grids(SubdivCCG &subdiv_ccg,
                                          Node &unode,
                                          const MutableSpan<bool> modified_grids)
{
  PRF_scope(ProfileCategory::Editor);
  if (unode.grid_hidden.is_empty()) {
    BKE_subdiv_ccg_grid_hidden_free(subdiv_ccg);
    return;
  }

  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);
  const Span<int> grids = unode.grids;
  for (const int i : grids.index_range()) {
    /* Swap the two bit spans. */
    MutableBoundedBitSpan a = unode.grid_hidden[i];
    MutableBoundedBitSpan b = grid_hidden[grids[i]];
    for (const int j : a.index_range()) {
      const bool value_a = a[j];
      const bool value_b = b[j];
      a[j].set(value_b);
      b[j].set(value_a);
    }
  }

  modified_grids.fill_indices(grids, true);
}

static void restore_hidden_face(Object &object,
                                Node &unode,
                                const MutableSpan<bool> modified_faces)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  const Span<int> face_indices = unode.face_indices;

  for (const int i : face_indices.index_range()) {
    const int face = face_indices[i];
    if (unode.face_hidden[i].test() != hide_poly.span[face]) {
      unode.face_hidden[i].set(!unode.face_hidden[i].test());
      hide_poly.span[face] = !hide_poly.span[face];
      modified_faces[face] = true;
    }
  }
  hide_poly.finish();
}

static void restore_color(Object &object,
                          StepData &step_data,
                          const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(mesh);

  for (std::unique_ptr<Node> &unode : step_data.nodes) {
    if (color_attribute.domain == bke::AttrDomain::Point && !unode->col.is_empty()) {
      color::swap_gathered_colors(
          unode->vert_indices.as_span().take_front(unode->unique_verts_num),
          color_attribute.span,
          unode->col);
    }
    else if (color_attribute.domain == bke::AttrDomain::Corner && !unode->loop_col.is_empty()) {
      color::swap_gathered_colors(unode->corner_indices, color_attribute.span, unode->loop_col);
    }

    modified_verts.fill_indices(unode->vert_indices.as_span(), true);
  }

  color_attribute.finish();
}

static void restore_color_grids(Object &object, StepData &step_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  MultiresModifierData *mmd = ss.multires_modifier;
  SubdivCCG *ccg = ss.subdiv_ccg;
  if (!mmd || !ccg || !mmd->sculpt_color_grids || mmd->sculpt_color_level < 1 ||
      mmd->sculpt_color_level > 13 ||
      mmd->sculpt_color_grids_num != ccg->grids_num)
  {
    return;
  }
  for (std::unique_ptr<Node> &unode : step_data.nodes) {
    const int side = unode->grid_color_side;
    if (side < 2 || side != bke::subdiv::grid_size_from_level(mmd->sculpt_color_level) ||
        unode->grid_color_present.size() != unode->grids.size())
    {
      continue;
    }
    const int area = side * side;
    if (unode->col.size() != unode->grids.size() * area) {
      continue;
    }
    for (const int i : unode->grids.index_range()) {
      const int grid_index = unode->grids[i];
      float4 *stored = unode->col.data() + i * area;
      const float (*current)[4] = bke::multires_layers::polypaint_grid(*mmd, grid_index);
      const bool had_color = unode->grid_color_present[i];
      const bool has_color = current != nullptr;
      if (had_color && has_color) {
        float (*writable)[4] = bke::multires_layers::polypaint_grid_ensure_master(*mmd,
                                                                                   grid_index);
        if (!writable) {
          continue;
        }
        for (int p = 0; p < area; p++) {
          for (int c = 0; c < 4; c++) {
            std::swap(writable[p][c], stored[p][c]);
          }
        }
      }
      else if (!had_color && has_color) {
        for (int p = 0; p < area; p++) {
          stored[p] = float4(current[p]);
        }
        if (!bke::multires_layers::polypaint_grid_discard(*mmd, grid_index)) {
          continue;
        }
      }
      else if (had_color && !has_color) {
        float (*created)[4] = bke::multires_layers::polypaint_grid_ensure_master(*mmd,
                                                                                  grid_index);
        if (!created) {
          continue;
        }
        for (int p = 0; p < area; p++) {
          for (int c = 0; c < 4; c++) {
            created[p][c] = stored[p][c];
          }
        }
      }
      unode->grid_color_present[i].set(has_color);
    }
  }
}

static void restore_mask_mesh(Object &object, Node &unode, const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh *mesh = BKE_object_get_original_mesh(&object);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);

  const Span<int> index = unode.vert_indices.as_span().take_front(unode.unique_verts_num);

  for (const int i : index.index_range()) {
    const int vert = index[i];
    if (mask.span[vert] != unode.mask[i]) {
      std::swap(mask.span[vert], unode.mask[i]);
      modified_verts[vert] = true;
    }
  }

  mask.finish();
}

static void restore_mask_grids(Object &object, Node &unode, const MutableSpan<bool> modified_grids)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG *subdiv_ccg = ss.subdiv_ccg;
  MutableSpan<float> masks = subdiv_ccg->masks;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(*subdiv_ccg);

  const Span<int> grids = unode.grids;
  MutableSpan<float> undo_mask = unode.mask;

  for (const int i : grids.index_range()) {
    MutableSpan data = masks.slice(bke::ccg::grid_range(key, grids[i]));
    MutableSpan undo_data = undo_mask.slice(bke::ccg::grid_range(key, i));
    for (const int offset : data.index_range()) {
      std::swap(data[offset], undo_data[offset]);
    }
  }

  modified_grids.fill_indices(unode.grids.as_span(), true);
}

static bool restore_face_sets(Object &object,
                              Node &unode,
                              const MutableSpan<bool> modified_face_set_faces)
{
  PRF_scope(ProfileCategory::Editor);
  const Span<int> face_indices = unode.face_indices;

  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(
      *id_cast<Mesh *>(object.data));
  bool modified = false;
  for (const int i : face_indices.index_range()) {
    const int face = face_indices[i];
    if (unode.face_sets[i] == face_sets.span[face]) {
      continue;
    }
    std::swap(unode.face_sets[i], face_sets.span[face]);
    modified_face_set_faces[face] = true;
    modified = true;
  }
  face_sets.finish();
  return modified;
}

static bool restore_face_sets_grids(Object &object, Node &unode)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  MultiresModifierData *mmd = ss.multires_modifier;
  if (!mmd || !mmd->sculpt_face_set_grids || unode.grid_face_set_side <= 0) {
    return false;
  }

  const int area = unode.grid_face_set_side * unode.grid_face_set_side;
  if (unode.grid_face_sets.size() != unode.grids.size() * area ||
      bke::subdiv::grid_size_from_level(mmd->sculpt_face_set_level) != unode.grid_face_set_side)
  {
    return false;
  }

  bool modified = false;
  int offset = 0;
  for (const int grid : unode.grids) {
    int *ids = bke::multires_layers::faceset_grid_ensure_dense(*mmd, grid);
    if (!ids) {
      offset += area;
      continue;
    }
    for (int i = 0; i < area; i++, offset++) {
      if (ids[i] != unode.grid_face_sets[offset]) {
        std::swap(ids[i], unode.grid_face_sets[offset]);
        modified = true;
      }
    }
  }
  return modified;
}

static void bmesh_restore_generic(StepData &step_data, Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  if (step_data.needs_undo()) {
    BM_log_undo(ss.bm, ss.bm_log);
    step_data.tag_needs_redo();
  }
  else {
    BM_log_redo(ss.bm, ss.bm_log);
    step_data.tag_needs_undo();
  }

  if (step_data.type == Type::Mask) {
    bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
    pbvh.tag_masks_changed(node_mask);
    bke::pbvh::update_mask_bmesh(*ss.bm, node_mask, pbvh);
  }
  else {
    BKE_sculptsession_free_pbvh(object);
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
    BM_mesh_normals_update(ss.bm);
  }
}

/* Create empty sculpt BMesh and enable logging. */
static void bmesh_enable(Object &object, const StepData &step_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  Mesh *mesh = id_cast<Mesh *>(object.data);

  BKE_sculptsession_free_pbvh(object);
  DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);

  /* Create empty BMesh and enable logging. */
  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = false;

  ss.bm = BM_mesh_create(&bm_mesh_allocsize_default, &bmesh_create_params);
  BM_data_layer_add_named(ss.bm, &ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");

  mesh->flag |= ME_SCULPT_DYNAMIC_TOPOLOGY;

  /* Restore the BMLog using saved entries. */
  ss.bm_log = BM_log_from_existing_entries_create(ss.bm, step_data.bmesh.bm_entry);
}

static void bmesh_handle_dyntopo_begin(bContext *C, StepData &step_data, Object &object)
{
  if (step_data.needs_undo()) {
    dyntopo::disable(C, &step_data);
    step_data.tag_needs_redo();
  }
  else /* needs_redo */ {
    SculptSession &ss = *object.runtime->sculpt_session;
    bmesh_enable(object, step_data);

    /* Restore the mesh from the first log entry. */
    BM_log_redo(ss.bm, ss.bm_log);

    step_data.tag_needs_undo();
  }
}

static void bmesh_handle_dyntopo_end(bContext *C, StepData &step_data, Object &object)
{
  if (step_data.needs_undo()) {
    SculptSession &ss = *object.runtime->sculpt_session;
    bmesh_enable(object, step_data);

    /* Restore the mesh from the last log entry. */
    BM_log_undo(ss.bm, ss.bm_log);

    step_data.tag_needs_redo();
  }
  else /* needs_redo */ {
    /* Disable dynamic topology sculpting. */
    dyntopo::disable(C, nullptr);
    step_data.tag_needs_undo();
  }
}

static void store_geometry_data(NodeGeometry *geometry, const Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  const Mesh *mesh = id_cast<const Mesh *>(object.data);

  BLI_assert(!geometry->is_initialized);
  geometry->is_initialized = true;

  geometry->attribute_storage = mesh->attribute_storage.wrap();
  CustomData_init_from(
      &mesh->vert_data, &geometry->vert_data, CD_MASK_MESH.vmask, mesh->verts_num);
  CustomData_init_from(
      &mesh->edge_data, &geometry->edge_data, CD_MASK_MESH.emask, mesh->edges_num);
  CustomData_init_from(
      &mesh->corner_data, &geometry->corner_data, CD_MASK_MESH.lmask, mesh->corners_num);
  CustomData_init_from(
      &mesh->face_data, &geometry->face_data, CD_MASK_MESH.pmask, mesh->faces_num);
  implicit_sharing::copy_shared_pointer(mesh->face_offset_indices,
                                        mesh->runtime->face_offsets_sharing_info,
                                        &geometry->face_offset_indices,
                                        &geometry->face_offsets_sharing_info);

  geometry->verts_num = mesh->verts_num;
  geometry->edges_num = mesh->edges_num;
  geometry->corners_num = mesh->corners_num;
  geometry->faces_num = mesh->faces_num;
}

static void restore_geometry_data(const NodeGeometry *geometry, Mesh *mesh)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(geometry->is_initialized);

  BKE_mesh_clear_geometry(mesh);

  mesh->verts_num = geometry->verts_num;
  mesh->edges_num = geometry->edges_num;
  mesh->corners_num = geometry->corners_num;
  mesh->faces_num = geometry->faces_num;
  mesh->totface_legacy = 0;

  mesh->attribute_storage.wrap() = geometry->attribute_storage.wrap();
  CustomData_init_from(
      &geometry->vert_data, &mesh->vert_data, CD_MASK_MESH.vmask, geometry->verts_num);
  CustomData_init_from(
      &geometry->edge_data, &mesh->edge_data, CD_MASK_MESH.emask, geometry->edges_num);
  CustomData_init_from(
      &geometry->corner_data, &mesh->corner_data, CD_MASK_MESH.lmask, geometry->corners_num);
  CustomData_init_from(
      &geometry->face_data, &mesh->face_data, CD_MASK_MESH.pmask, geometry->faces_num);
  implicit_sharing::copy_shared_pointer(geometry->face_offset_indices,
                                        geometry->face_offsets_sharing_info,
                                        &mesh->face_offset_indices,
                                        &mesh->runtime->face_offsets_sharing_info);
}

static void geometry_free_data(NodeGeometry *geometry)
{
  PRF_scope(ProfileCategory::Editor);
  CustomData_free(&geometry->vert_data);
  CustomData_free(&geometry->edge_data);
  CustomData_free(&geometry->corner_data);
  CustomData_free(&geometry->face_data);
  implicit_sharing::free_shared_data(&geometry->face_offset_indices,
                                     &geometry->face_offsets_sharing_info);
}

static void restore_geometry(StepData &step_data, Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  BKE_sculptsession_free_pbvh(object);
  DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);

  Mesh *mesh = id_cast<Mesh *>(object.data);

  if (step_data.needs_undo()) {
    restore_geometry_data(&step_data.geometry_original, mesh);
    step_data.tag_needs_redo();
  }
  else {
    restore_geometry_data(&step_data.geometry_modified, mesh);
    step_data.tag_needs_undo();
  }
}

/* Handle all dynamic-topology updates
 *
 * Returns true if this was a dynamic-topology undo step, otherwise
 * returns false to indicate the non-dyntopo code should run. */
static int bmesh_restore(bContext *C, Depsgraph &depsgraph, StepData &step_data, Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  switch (step_data.type) {
    case Type::DyntopoBegin:
      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      bmesh_handle_dyntopo_begin(C, step_data, object);
      return true;

    case Type::DyntopoEnd:
      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      bmesh_handle_dyntopo_end(C, step_data, object);
      return true;
    default:
      if (ss.bm_log) {
        BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
        bmesh_restore_generic(step_data, object);
        return true;
      }
      break;
  }

  return false;
}

void restore_from_bmesh_enter_geometry(const StepData &step_data, Mesh &mesh)
{
  restore_geometry_data(&step_data.bmesh.geometry_enter, &mesh);
}

bool has_bmesh_log_entry()
{
  return get_step_data()->bmesh.bm_entry;
}

/* Geometry updates (such as Apply Base, for example) will re-evaluate the object and refine its
 * Subdiv descriptor. Upon undo it is required that mesh, grids, and subdiv all stay consistent
 * with each other. This means that when geometry coordinate changes the undo should refine the
 * subdiv to the new coarse mesh coordinates. Tricky part is: this needs to happen without using
 * dependency graph tag: tagging object for geometry update will either loose sculpted data from
 * the sculpt grids, or will wrongly "commit" them to the CD_MDISPS.
 *
 * So what we do instead is do minimum object evaluation to get base mesh coordinates for the
 * multires modifier input. While this is expensive, it is less expensive than dependency graph
 * evaluation and is only happening when geometry coordinates changes on undo.
 *
 * Note that the dependency graph is ensured to be evaluated prior to the undo step is decoded,
 * so if the object's modifier stack references other object it is all fine. */
static void refine_subdiv(Depsgraph *depsgraph,
                          const SculptSession &ss,
                          Object &object,
                          bke::subdiv::Subdiv *subdiv)
{
  Array<float3> deformed_verts = BKE_multires_create_deformed_base_mesh_vert_coords(
      depsgraph, &object, ss.multires_modifier);

  bke::subdiv::eval_refine_from_mesh(subdiv, id_cast<const Mesh *>(object.data), deformed_verts);
}

static void sculpt_layer_undo_metadata_restore(bContext *C,
                                               Depsgraph *depsgraph,
                                               Object &object,
                                               const StepData &step_data)
{
  if (!sculpt_layer_undo_metadata_enabled() || step_data.layer_metadata.is_empty()) {
    return;
  }
  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object.modifiers) {
    if (md.type == eModifierType_Multires && md.name == step_data.layer_metadata_modifier) {
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
      break;
    }
  }
  if (!mmd || mmd->sculpt_layers.count() != step_data.layer_metadata.size()) {
    std::printf("[LAYER_UNDO_METADATA] mode=SKIP reason=STACK_MISMATCH\n");
    return;
  }
  int index = 0;
  bool changed = false;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    const SculptLayerUndoMetadata &saved = step_data.layer_metadata[index++];
    if (saved.name != layer.name) {
      std::printf("[LAYER_UNDO_METADATA] mode=SKIP reason=LAYER_MISMATCH\n");
      return;
    }
    changed |= layer.strength != saved.strength ||
               ((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0) != saved.visible;
  }
  if (!changed) {
    return;
  }
  /* Flush current composition before changing its weights; never keep stale grid references. */
  if (!bke::multires_layers::commit(object)) {
    std::printf("[LAYER_UNDO_METADATA] mode=SKIP reason=COMMIT_FAILED\n");
    return;
  }
  index = 0;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    const SculptLayerUndoMetadata &saved = step_data.layer_metadata[index++];
    layer.strength = saved.strength;
    if (saved.visible) {
      layer.flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
    }
    else {
      layer.flag &= ~MULTIRES_SCULPT_LAYER_VISIBLE;
    }
  }
  bke::multires_layers::weight_hot_delta_cache_invalidate();
  multires_force_sculpt_rebuild(&object);
  DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
  BKE_scene_graph_evaluated_ensure(depsgraph, CTX_data_main(C));
  WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, &object);
  std::printf("[LAYER_UNDO_METADATA] mode=RESTORE layers=%d\n",
              int(step_data.layer_metadata.size()));
}



enum class SculptLayerPersistentRestoreResult {
  NotApplicable,
  PositionSwap,
  /* SCULPT_LAYER_UNDO_LOWER_NATIVE_V34: CCG is authoritative until normal flush. */
  DeferredPositionSwap,
  Refreshed,
};

static bool sculpt_layer_undo_persistent_payload_swap(MultiresSculptLayer &layer,
                                                       PositionUndoStorage &undo_data)
{
  /* The worker publishes both compressed arrays together. Inspecting their lengths before the
   * acquire/wait can select a fallback or an empty transaction while compression is in flight. */
  undo_data.ensure_compression_complete();
  if (!undo_data.sculpt_layer_undo || undo_data.sculpt_layer_grid_area <= 0 ||
      undo_data.compressed_layer_disps.size() != undo_data.compressed_indices.size())
  {
    return false;
  }

  const int area = undo_data.sculpt_layer_grid_area;
  const int side = bke::subdiv::grid_size_from_level(layer.level);
  if (side < 2 || side * side != area || layer.disps == nullptr || layer.grids_num <= 0) {
    return false;
  }

  struct LocalData {
    Vector<std::byte> compress_buffer;
    Vector<std::byte> filter_buffer;
    Vector<int> grids;
    Vector<float3> deltas;
  } tls;

  /* First validate every compressed node and every live descriptor. Nothing persistent is changed
   * until the complete sparse transaction is known to be valid. */
  for (const int node_i : undo_data.compressed_indices.index_range()) {
    compression::filter_decompress<int>(
        undo_data.compressed_indices[node_i], tls.compress_buffer, tls.grids);
    compression::filter_decompress<float3>(
        undo_data.compressed_layer_disps[node_i], tls.compress_buffer, tls.deltas);
    if (tls.grids.is_empty() ||
        tls.deltas.size() != int64_t(tls.grids.size()) * int64_t(area))
    {
      return false;
    }
    for (const int grid : tls.grids) {
      if (grid < 0 || grid >= layer.grids_num || layer.disps[grid].level != layer.level ||
          layer.disps[grid].totdisp != area)
      {
        return false;
      }
    }
  }

  /* Exchange the unweighted persistent layer deltas. The undo buffer becomes the redo buffer in
   * the same way compressed_positions does for native Sculpt undo. A sparse null live payload is
   * materialized only for grids that belong to this stroke. */
  for (const int node_i : undo_data.compressed_indices.index_range()) {
    compression::filter_decompress<int>(
        undo_data.compressed_indices[node_i], tls.compress_buffer, tls.grids);
    compression::filter_decompress<float3>(
        undo_data.compressed_layer_disps[node_i], tls.compress_buffer, tls.deltas);

    for (const int grid_i : tls.grids.index_range()) {
      const int grid = tls.grids[grid_i];
      float (*live)[3] = bke::multires_layers::grid_ensure(layer, grid);
      if (live == nullptr) {
        return false;
      }
      MutableSpan<float3> saved = tls.deltas.as_mutable_span().slice(
          int64_t(grid_i) * int64_t(area), area);
      for (const int i : saved.index_range()) {
        const float3 current = float3(live[i]);
        live[i][0] = saved[i].x;
        live[i][1] = saved[i].y;
        live[i][2] = saved[i].z;
        saved[i] = current;
      }
    }

    compression::filter_compress<float3>(
        tls.deltas.as_span(), tls.filter_buffer, tls.compress_buffer);
    if (tls.compress_buffer.is_empty()) {
      return false;
    }
    undo_data.compressed_layer_disps[node_i] = tls.compress_buffer.as_span();
  }

  return true;
}

static bool sculpt_layer_undo_persistent_frame_matches(const MultiresModifierData &mmd,
                                                       const StepData &step_data)
{
  if (mmd.sculptlvl != step_data.layer_frame_level ||
      mmd.sculpt_layer_active != step_data.layer_frame_active ||
      mmd.totlvl != step_data.layer_frame_top_level ||
      mmd.sculpt_layers.count() != step_data.layer_frame.size())
  {
    return false;
  }

  int i = 0;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    const SculptLayerUndoFrameItem &saved = step_data.layer_frame[i++];
    if (saved.name != layer.name || saved.strength != layer.strength ||
        saved.visible != ((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0))
    {
      return false;
    }
  }
  return true;
}

static SculptLayerPersistentRestoreResult sculpt_layer_undo_persistent_restore(
    Depsgraph *depsgraph, Object &object, StepData &step_data)
{
  if (sculpt_layer_undo_ccg_enabled() || !sculpt_layer_undo_persistent_enabled() ||
      step_data.type != Type::Position ||
      !step_data.position_step_storage || !step_data.position_step_storage->sculpt_layer_undo ||
      step_data.layer_frame.is_empty() || step_data.layer_frame_active < 0 ||
      step_data.layer_frame_active >= step_data.layer_frame.size())
  {
    return SculptLayerPersistentRestoreResult::NotApplicable;
  }

  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object.modifiers) {
    if (md.type == eModifierType_Multires && md.name == step_data.layer_frame_modifier) {
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
      break;
    }
  }
  if (!mmd || mmd->totlvl != step_data.layer_frame_top_level) {
    std::printf("[LAYER_UNDO_V3] mode=FALLBACK reason=MODIFIER_OR_TOP_LEVEL_CHANGED\n");
    return SculptLayerPersistentRestoreResult::NotApplicable;
  }

  const std::string &recorded_layer_name =
      step_data.layer_frame[step_data.layer_frame_active].name;
  MultiresSculptLayer *recorded_layer = nullptr;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (layer.name == recorded_layer_name) {
      recorded_layer = &layer;
      break;
    }
  }
  if (!recorded_layer || recorded_layer->level != mmd->totlvl ||
      !object.runtime->sculpt_session || !object.runtime->sculpt_session->subdiv_ccg)
  {
    std::printf("[LAYER_UNDO_V3] mode=FALLBACK reason=LAYER_OR_SESSION_CHANGED\n");
    return SculptLayerPersistentRestoreResult::NotApplicable;
  }

  const bool same_position_frame =
      sculpt_layer_undo_persistent_frame_matches(*mmd, step_data) &&
      topology_matches(step_data, object);

  /* SCULPT_LAYER_UNDO_LOWER_NATIVE_V34
   * Lower-level commit uses a global, detail-preserving reshape. A per-node
   * top-level payload snapshot cannot undo every grid that this reshape may
   * modify. When the composition/layout is identical, native compressed CCG
   * position exchange is sufficient. Keep it pending, just like a stroke;
   * a later normal flush persists the restored surface in the active layer.
   * Both Undo and Redo use this path. Do not exchange persistent payloads,
   * re-evaluate the CCG, or report it as already synchronized here. */
  const char *lower_native_env = std::getenv("BLENDER_SCULPT_LAYER_UNDO_LOWER_NATIVE");
  const SculptSession &live_ss = *object.runtime->sculpt_session;
  if (same_position_frame && mmd->sculptlvl > 0 && mmd->sculptlvl < mmd->totlvl &&
      live_ss.subdiv_ccg->level == mmd->sculptlvl && !live_ss.cache &&
      !live_ss.filter_cache && !live_ss.subdiv_ccg->dirty.hidden &&
      !live_ss.subdiv_ccg->dirty.masks &&
      (!lower_native_env || lower_native_env[0] != '0'))
  {
    std::printf("[LAYER_UNDO_V34] mode=LOWER_NATIVE_PENDING level=%d top=%d strength=%.6f\n",
                mmd->sculptlvl, mmd->totlvl, recorded_layer->strength);
    return SculptLayerPersistentRestoreResult::DeferredPositionSwap;
  }

  /* The live CCG is the authoritative post-stroke surface until it is flushed. Convert that
   * surface to the active persistent layer exactly once before exchanging persistent undo data. */
  if (!bke::multires_layers::commit(object)) {
    std::printf("[LAYER_UNDO_V3] mode=FALLBACK reason=COMMIT_FAILED\n");
    return SculptLayerPersistentRestoreResult::NotApplicable;
  }

  if (!sculpt_layer_undo_persistent_payload_swap(
          *recorded_layer, *step_data.position_step_storage))
  {
    std::printf("[LAYER_UNDO_V3] mode=FALLBACK reason=PAYLOAD_SWAP_FAILED\n");
    return SculptLayerPersistentRestoreResult::NotApplicable;
  }
  bke::multires_layers::weight_hot_delta_cache_invalidate(recorded_layer);

  if (same_position_frame) {
    /* At the top level the native CCG position swap is a cheap exact display update once the
     * persistent payload has been exchanged. At lower Sculpt Levels, however, the live CCG is only
     * a sampled representation of the authoritative top-level layer payload. Swapping both the
     * persistent payload and the lower-level CCG leaves two independent undo representations in
     * play. Recompose the current lower-level CCG directly from the restored persistent payload
     * instead, without changing sculptlvl or rebuilding Multires. */
    if (mmd->sculptlvl < mmd->totlvl || !step_data.layer_position_buffers_match_payload) {
      if (!multires_refresh_sculpt_ccg(&object, mmd)) {
        /* Keep the operation transactional: put the persistent payload back so the legacy path can
         * still handle the step. */
        sculpt_layer_undo_persistent_payload_swap(*recorded_layer,
                                                  *step_data.position_step_storage);
        bke::multires_layers::weight_hot_delta_cache_invalidate(recorded_layer);
        std::printf("[LAYER_UNDO_V33] mode=FALLBACK reason=LOWER_LEVEL_REFRESH_FAILED level=%d\n",
                    mmd->sculptlvl);
        return SculptLayerPersistentRestoreResult::NotApplicable;
      }

      SculptSession &ss = *object.runtime->sculpt_session;
      ss.pivot_pos = step_data.pivot_pos;
      ss.pivot_rot = step_data.pivot_rot;
      DEG_id_tag_update(&object.id, ID_RECALC_SYNC_TO_EVAL);
      WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object);
      std::printf(
          "[LAYER_UNDO_V35] mode=PAYLOAD_REFRESH level=%d top=%d strength=%.6f "
          "position_swap=0\n",
          mmd->sculptlvl,
          mmd->totlvl,
          recorded_layer->strength);
      step_data.layer_position_buffers_match_payload = false;
      return SculptLayerPersistentRestoreResult::Refreshed;
    }

    std::printf(
        "[LAYER_UNDO_V3] mode=PERSISTENT_SWAP level=%d strength=%.6f position_swap=1\n",
        mmd->sculptlvl,
        recorded_layer->strength);
    return SculptLayerPersistentRestoreResult::PositionSwap;
  }

  /* Strength/visibility/current Sculpt Level may have changed since the stroke. PositionUndoStorage
   * contains the old composed surface and is therefore the wrong display representation. Recompose
   * from the now-restored persistent payload using the CURRENT frame, leaving user metadata intact. */
  if (!multires_refresh_sculpt_ccg(&object, mmd)) {
    /* Restore the persistent payload to its pre-call state so the legacy path can still run. */
    sculpt_layer_undo_persistent_payload_swap(*recorded_layer,
                                              *step_data.position_step_storage);
    bke::multires_layers::weight_hot_delta_cache_invalidate(recorded_layer);
    std::printf("[LAYER_UNDO_V3] mode=FALLBACK reason=DISPLAY_REFRESH_FAILED\n");
    return SculptLayerPersistentRestoreResult::NotApplicable;
  }

  SculptSession &ss = *object.runtime->sculpt_session;
  ss.pivot_pos = step_data.pivot_pos;
  ss.pivot_rot = step_data.pivot_rot;
  DEG_id_tag_update(&object.id, ID_RECALC_SYNC_TO_EVAL);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object);
  std::printf(
      "[LAYER_UNDO_V3] mode=PERSISTENT_REFRESH recorded_level=%d display_level=%d strength=%.6f\n",
      step_data.layer_frame_level,
      mmd->sculptlvl,
      recorded_layer->strength);
  step_data.layer_position_buffers_match_payload = false;
  return SculptLayerPersistentRestoreResult::Refreshed;
}


/* Grid undo positions are expressed in the composition at stroke time.
 * Convert back to persistent layer deltas in that same composition, then
 * display the result using the current weights. Undo/redo both exchange
 * coordinates in the saved frame, so changing strength cannot amplify them.
 * This intentionally leaves property undo to its own undo steps. */
static bool sculpt_layer_undo_frame_restore(bContext *C,
                                           Depsgraph *depsgraph,
                                           Object &object,
                                           StepData &step_data)
{
  /* This function is reached only when persistent restore did not handle the step.
   * Native coordinates are still tied to their captured composition, regardless of why
   * persistent restore declined (pending capture, invalid payload, or failed commit). */
  const bool coordinate_frame_fallback = sculpt_layer_undo_ccg_enabled() ||
                                         sculpt_layer_undo_persistent_enabled();
  if ((!sculpt_layer_undo_frame_enabled() && !step_data.native_multires_base_undo &&
       !coordinate_frame_fallback) ||
      step_data.type != Type::Position ||
      step_data.layer_frame.is_empty())
  {
    return false;
  }
  const auto reject = [](const char *reason) {
    std::printf("[LAYER_UNDO_FRAME] mode=SKIP reason=%s\n", reason);
    return true; /* Do not fall through to coordinate undo in an incompatible frame. */
  };
  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object.modifiers) {
    if (md.type == eModifierType_Multires && md.name == step_data.layer_frame_modifier) {
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
      break;
    }
  }
  if (!mmd || mmd->sculpt_layers.count() != step_data.layer_frame.size() ||
      mmd->totlvl != step_data.layer_frame_top_level || !step_data.position_step_storage ||
      step_data.layer_frame_active < 0 ||
      step_data.layer_frame_active >= step_data.layer_frame.size())
  {
    return reject("STACK_MISMATCH");
  }

  /* SCULPT_LAYER_UNDO_CROSS_LEVEL_V2
   *
   * Position undo is stored in the CCG layout used by the stroke. A later
   * Sculpt Levels change can therefore not swap those coordinates directly
   * into the current CCG. The level setter commits pending layer edits before
   * changing level (see rna_MultiresModifier_sculpt_levels_set), so it is safe
   * to temporarily rebuild the live CCG at the recorded level, perform the
   * normal swap+commit there, then return to the user's display level.
   *
   * This path is intentionally only used for cross-level undo/redo. Same-level
   * strokes keep the cheap STANDARD_SWAP path and never pay for a rebuild. */
  const int display_level = mmd->sculptlvl;
  const bool cross_level = display_level != step_data.layer_frame_level;
  const auto rebuild_level = [&](const int level) -> bool {
    if (mmd->sculptlvl == level && object.runtime->sculpt_session &&
        object.runtime->sculpt_session->subdiv_ccg &&
        object.runtime->sculpt_session->subdiv_ccg->grids_num == step_data.grids.grids_num &&
        object.runtime->sculpt_session->subdiv_ccg->grid_size == step_data.grids.grid_size)
    {
      return true;
    }
    /* Flush while the old CCG still matches its level, then rebuild at the
     * requested level. Never change sculptlvl before the flush. */
    if (!multires_force_sculpt_rebuild(&object)) {
      return false;
    }
    mmd->sculptlvl = std::clamp(level, 0, int(mmd->totlvl));
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
    BKE_scene_graph_evaluated_ensure(depsgraph, CTX_data_main(C));
    bke::pbvh::Tree &level_pbvh = bke::object::pbvh_ensure(*depsgraph, object);
    SculptSession *level_ss = object.runtime->sculpt_session;
    return level_pbvh.type() == bke::pbvh::Type::Grids && level_ss && level_ss->subdiv_ccg &&
           level_ss->subdiv_ccg->grids_num == step_data.grids.grids_num &&
           level_ss->subdiv_ccg->grid_size == step_data.grids.grid_size;
  };
  const auto restore_display_level = [&]() -> bool {
    if (!cross_level) {
      return true;
    }
    /* The recorded-level swap has already been committed to persistent layer
     * data. Rebuild the user's original level from that payload. */
    if (!multires_force_sculpt_rebuild(&object)) {
      /* Keep the user-visible level selection coherent even if the rebuild
       * itself failed; the geometry tag below lets the normal fallback path
       * rebuild it later. */
      mmd->sculptlvl = std::clamp(display_level, 0, int(mmd->totlvl));
      DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object);
      return false;
    }
    mmd->sculptlvl = std::clamp(display_level, 0, int(mmd->totlvl));
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
    BKE_scene_graph_evaluated_ensure(depsgraph, CTX_data_main(C));
    bke::object::pbvh_ensure(*depsgraph, object);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object);
    return true;
  };
  Vector<SculptLayerUndoFrameItem> current;
  int index = 0;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    const SculptLayerUndoFrameItem &saved = step_data.layer_frame[index++];
    if (saved.name != layer.name || !std::isfinite(saved.strength) ||
        !std::isfinite(layer.strength))
    {
      return reject("LAYER_MISMATCH");
    }
    /* The in-place CCG refresh does not refine a changed L0 base cage. */
    if (layer.base_deltas &&
        (saved.strength != layer.strength ||
         saved.visible != ((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0)))
    {
      return reject("L0_FRAME_CHANGED");
    }
    current.append({layer.name, layer.strength,
                    (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0});
  }
  const auto &recorded_active = step_data.layer_frame[step_data.layer_frame_active];
  if (!step_data.native_multires_base_undo &&
      (!recorded_active.visible || std::abs(recorded_active.strength) < 1e-6f)) {
    return reject("INVALID_RECORDED_WEIGHT");
  }
  if (cross_level && !rebuild_level(step_data.layer_frame_level)) {
    /* Best effort to leave the object at the level selected by the user. */
    restore_display_level();
    return reject("RECORDED_LEVEL_REBUILD_FAILED");
  }
  const auto reject_runtime = [&](const char *reason) {
    if (cross_level) {
      restore_display_level();
    }
    return reject(reason);
  };
  SculptSession *ss = object.runtime->sculpt_session;
  if (!ss || !ss->subdiv_ccg || ss->cache || ss->filter_cache ||
      ss->subdiv_ccg->grids_num != step_data.grids.grids_num ||
      ss->subdiv_ccg->grid_size != step_data.grids.grid_size ||
      ss->subdiv_ccg->dirty.hidden || ss->subdiv_ccg->dirty.masks)
  {
    /* LAYER_UNDO_SESSION_DIAGNOSTIC_V1: distinguish an unavailable CCG from
     * dirty mask/hide data or an actual layout mismatch. Do not weaken the
     * coordinate-frame safety gate. */
    std::printf("[LAYER_UNDO_SESSION] session=%d ccg=%d cache=%d filter=%d "
                "grids=%d/%d side=%d/%d hidden_dirty=%d masks_dirty=%d\n",
                int(ss != nullptr), int(ss && ss->subdiv_ccg), int(ss && ss->cache),
                int(ss && ss->filter_cache),
                ss && ss->subdiv_ccg ? ss->subdiv_ccg->grids_num : -1,
                step_data.grids.grids_num,
                ss && ss->subdiv_ccg ? ss->subdiv_ccg->grid_size : -1,
                step_data.grids.grid_size,
                int(ss && ss->subdiv_ccg && ss->subdiv_ccg->dirty.hidden),
                int(ss && ss->subdiv_ccg && ss->subdiv_ccg->dirty.masks));
    return reject_runtime("SESSION_MISMATCH");
  }
  /* SCULPT_LAYER_UNDO_FAST_STANDARD_V2
   *
   * The expensive Frame-v1 path commits the live CCG, changes composition,
   * swaps coordinates, commits again, then evaluates the CCG again. That is
   * only required when the composition really changed.
   *
   * When layer order/names, active layer, level, strength and visibility are
   * identical to the frame recorded for this stroke, Blender's native
   * Multires Position undo is already the exact operation we need:
   *
   *   compressed position swap -> PBVH dirty nodes -> pending Multires CCG
   *
   * Do not commit or rebuild here. Keeping the restored CCG dirty is the same
   * model used by a normal sculpt stroke; the next operation that needs
   * persistent layer data will commit it. This is especially important at
   * lower Sculpt Levels, where forcing a layer commit + CCG refresh turns a
   * cheap local undo into a global reshape/evaluation.
   *
   * A/B fallback:
   *   BLENDER_SCULPT_LAYER_UNDO_FAST_STANDARD=0
   */
  const int current_active = mmd->sculpt_layer_active;
  const char *same_frame_env = std::getenv("BLENDER_SCULPT_LAYER_UNDO_SAME_FRAME");
  bool same_frame = !cross_level &&
                    (step_data.native_multires_base_undo || coordinate_frame_fallback ||
                     (same_frame_env && same_frame_env[0] == '1')) &&
                    current_active == step_data.layer_frame_active;
  for (const int i : current.index_range()) {
    same_frame &= current[i].strength == step_data.layer_frame[i].strength &&
                  current[i].visible == step_data.layer_frame[i].visible;
  }

  const char *fast_standard_env = std::getenv("BLENDER_SCULPT_LAYER_UNDO_FAST_STANDARD");
  const bool fast_standard =
      fast_standard_env == nullptr || fast_standard_env[0] == '\0' ||
      fast_standard_env[0] != '0';

  if (same_frame && fast_standard) {
    std::printf(
        "[LAYER_UNDO_V2] mode=STANDARD_SWAP level=%d layers=%d pending_preserved=1\n",
        mmd->sculptlvl,
        int(current.size()));
    /* Not handled by Frame-v1: deliberately fall through to Blender's native
     * Position undo below. Do not consume the step here. */
    return false;
  }

  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(*depsgraph, object);
  if (pbvh.type() != bke::pbvh::Type::Grids || !bke::multires_layers::commit(object)) {
    return reject_runtime("PREPARE_FAILED");
  }

  /* Legacy Frame-v1 A/B path. After Commit the journal is expected to be
   * clean; preserve its previous SAME_FRAME optimization when the V2 bypass
   * is explicitly disabled. */
  if (same_frame) {
    const SubdivCCG &live_ccg = *object.runtime->sculpt_session->subdiv_ccg;
    std::lock_guard<std::mutex> lock(live_ccg.sculpt_dirty_position_grids_mutex);
    same_frame &= !live_ccg.dirty.coords && !live_ccg.dirty.hidden &&
                  !live_ccg.dirty.masks && live_ccg.sculpt_dirty_position_grids.is_empty();
  }
  const auto set_frame = [&](const Span<SculptLayerUndoFrameItem> frame, const int active) {
    int i = 0;
    for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
      const SculptLayerUndoFrameItem &item = frame[i++];
      layer.strength = item.strength;
      if (item.visible) {
        layer.flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
      }
      else {
        layer.flag &= ~MULTIRES_SCULPT_LAYER_VISIBLE;
      }
    }
    mmd->sculpt_layer_active = active;
    bke::multires_layers::weight_hot_delta_cache_invalidate();
  };
  const auto finish = [&]() {
    set_frame(current.as_span(), current_active);
    if (cross_level) {
      /* No need to re-evaluate the recorded level just before replacing it
       * with the user's display level. */
      return true;
    }
    const bool refreshed = multires_refresh_sculpt_ccg(&object, mmd);
    DEG_id_tag_update(&object.id, refreshed ? ID_RECALC_SYNC_TO_EVAL : ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object);
    return refreshed;
  };
  if (!same_frame) {
    set_frame(step_data.layer_frame.as_span(), step_data.layer_frame_active);
    if (!multires_refresh_sculpt_ccg(&object, mmd)) {
      finish();
      return reject_runtime("RECORDED_FRAME_REFRESH_FAILED");
    }
  }
  else {
    /* Match the journal cleanup done by the skipped refresh. No coordinates,
     * normals or bounds are modified here; the final refresh still runs. */
    pbvh.clear_changes();
  }
  /* Refresh is in-place; it keeps the CCG and PBVH alive and clears its journal.
   * Reacquire positions after refresh and mark only exchanged grids for commit. */
  SubdivCCG &ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(ccg);
  Array<bool> modified_grids(ccg.grids_num, false);
  restore_position_grids(ccg.positions, key, *step_data.position_step_storage, modified_grids);
  IndexMaskMemory memory;
  const IndexMask all_nodes = bke::pbvh::all_leaf_nodes(pbvh, memory);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const IndexMask changed_nodes = IndexMask::from_predicate(
      all_nodes, memory,
      [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
      exec_mode::grain_size(1));
  pbvh.tag_positions_changed(changed_nodes);
  multires_mark_as_modified(depsgraph, &object, MULTIRES_COORDS_MODIFIED);
  const bool committed = bke::multires_layers::commit(object);
  if (!committed) {
    /* Commit retains pending coordinates on failure. Undo our coordinate swap
     * before refreshing, preserving both the payload and the redo storage. */
    restore_position_grids(ccg.positions, key, *step_data.position_step_storage, modified_grids);
  }
  const bool refreshed = finish();
  if (!committed) {
    return reject_runtime("COMMIT_FAILED_ROLLED_BACK");
  }
  if (!refreshed) {
    return reject_runtime("DISPLAY_REFRESH_FAILED");
  }
  if (!restore_display_level()) {
    return reject("DISPLAY_LEVEL_RESTORE_FAILED");
  }
  ss = object.runtime->sculpt_session;
  ss->pivot_pos = step_data.pivot_pos;
  ss->pivot_rot = step_data.pivot_rot;
  std::printf("[LAYER_UNDO_FRAME] mode=RESTORE layers=%d active=%d weights=PRESERVED\n",
              int(current.size()), step_data.layer_frame_active);
  std::printf("[LAYER_UNDO_FRAME_FAST] mode=%s skipped_eval=%d\n",
              same_frame ? "SAME_FRAME" : (cross_level ? "CROSS_LEVEL" : "FULL"),
              same_frame ? 1 : 0);
  if (cross_level) {
    std::printf("[LAYER_UNDO_CROSS_LEVEL] recorded=%d display=%d restored=1\n",
                step_data.layer_frame_level,
                display_level);
  }
  return true;
}


static void restore_list(bContext *C, Depsgraph *depsgraph, StepData &step_data)
{
  PRF_scope(ProfileCategory::Editor);
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object &object = *BKE_view_layer_active_object_get(view_layer);
  if (step_data.object_name != object.id.name) {
    return;
  }

  const SculptLayerPersistentRestoreResult persistent_result =
      sculpt_layer_undo_persistent_restore(depsgraph, object, step_data);
  if (persistent_result == SculptLayerPersistentRestoreResult::Refreshed) {
    return;
  }
  const bool persistent_position_swap =
      persistent_result == SculptLayerPersistentRestoreResult::PositionSwap;
  const bool layer_position_swap = persistent_position_swap ||
      persistent_result == SculptLayerPersistentRestoreResult::DeferredPositionSwap;

  /* V3 owns Position undo whenever persistent layer deltas were exchanged. Do not restore
   * Strength/Visibility metadata from the stroke step: those are independent user properties. */
  if (!layer_position_swap) {
    if (sculpt_layer_undo_frame_restore(C, depsgraph, object, step_data)) {
      return;
    }
    sculpt_layer_undo_metadata_restore(C, depsgraph, object, step_data);
  }
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(*depsgraph, object);

  if (step_data.type == Type::Position && !step_data.base_layer_name.empty()) {
    MultiresModifierData *mmd = nullptr;
    for (ModifierData &md : object.modifiers) {
      if (md.type == eModifierType_Multires && md.name == step_data.base_modifier_name) {
        mmd = reinterpret_cast<MultiresModifierData *>(&md);
        break;
      }
    }
    if (!mmd) {
      return;
    }
    for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
      if (layer.name != step_data.base_layer_name) {
        continue;
      }
      if (!layer.base_deltas || layer.base_verts_num != step_data.base_layer_deltas.size()) {
        return;
      }
      MutableSpan<float3> deltas(reinterpret_cast<float3 *>(layer.base_deltas), layer.base_verts_num);
      for (const int vertex : deltas.index_range()) {
        std::swap(deltas[vertex], step_data.base_layer_deltas[vertex]);
      }
      multires_force_sculpt_rebuild(&object);
      DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object);
      return;
    }
    return;
  }

  /* Restore pivot. */
  ss.pivot_pos = step_data.pivot_pos;
  ss.pivot_rot = step_data.pivot_rot;

  if (bmesh_restore(C, *depsgraph, step_data, object)) {
    return;
  }

  /* Switching to sculpt mode does not push a particular type.
   * See #124484. */
  /* TODO: Add explicit type for switching into Sculpt Mode. */
  if (step_data.type == Type::None && step_data.nodes.is_empty()) {
    return;
  }

  /* Adding multires via the `subdivision_set` operator results in the subsequent undo step
   * not correctly performing a global undo step; we exit early here to avoid crashing.
   * See: #131478 */
  const bool multires_undo_step = use_multires_undo(step_data, ss);
  if ((multires_undo_step && pbvh.type() != bke::pbvh::Type::Grids) ||
      (!multires_undo_step && pbvh.type() != bke::pbvh::Type::Mesh))
  {
    CLOG_WARN(&LOG,
              "Undo step type and sculpt geometry type do not match: skipping undo state restore");
    return;
  }

  const bool tag_update = ID_REAL_USERS(object.data) > 1 ||
                          !BKE_sculptsession_use_pbvh_draw(&object, rv3d) || ss.shapekey_active ||
                          ss.deform_modifiers_active;

  switch (step_data.type) {
    case Type::None: {
      BLI_assert_unreachable();
      break;
    }
    case Type::Position: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object)) {
        return;
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

        Array<bool> modified_grids(subdiv_ccg.grids_num, false);
        restore_position_grids(
            subdiv_ccg.positions, key, *step_data.position_step_storage, modified_grids);

        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
            exec_mode::grain_size(1));
        pbvh.tag_positions_changed(changed_nodes);
        if (!persistent_position_swap) {
          multires_mark_as_modified(depsgraph, &object, MULTIRES_COORDS_MODIFIED);
        }
        else {
          /* Persistent layer data already matches the restored CCG. Marking MULTIRES_COORDS_MODIFIED
           * here would make a later flush solve the same stroke a second time (Strength 0.5 -> the
           * characteristic doubled/1.0-looking result). Keep only PBVH draw/bounds/normals dirty. */
          std::printf("[LAYER_UNDO_V3] mode=CCG_SWAP_CLEAN persistent_synced=1\n");
        }
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        if (!restore_active_shape_key(*C, *depsgraph, step_data, object)) {
          return;
        }
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        Array<bool> modified_verts(mesh.verts_num, false);
        restore_position_mesh(object, *step_data.position_step_storage, modified_verts);

        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              return indices_contain_true(modified_verts, nodes[i].all_verts());
            },
            exec_mode::grain_size(1));
        pbvh.tag_positions_changed(changed_nodes);
      }

      if (tag_update) {
        Mesh &mesh = *id_cast<Mesh *>(object.data);
        mesh.tag_positions_changed();
        BKE_sculptsession_free_deformMats(&ss);
      }
      else {
        Mesh &mesh = *id_cast<Mesh *>(object.data);
        /* The BVH normals recalculation that will happen later (caused by
         * `pbvh.tag_positions_changed`) won't recalculate the face corner normals.
         * We need to manually clear that cache. */
        mesh.runtime->corner_normals_cache.tag_dirty();
      }
      pbvh.update_bounds(*depsgraph, object);
      bke::pbvh::store_bounds_orig(pbvh);
      break;
    }
    case Type::HideVert: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object)) {
        return;
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        Array<bool> modified_grids(subdiv_ccg.grids_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_vert_visibility_grids(subdiv_ccg, *unode, modified_grids);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        Array<bool> modified_verts(mesh.verts_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_vert_visibility_mesh(object, *unode, modified_verts);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              return indices_contain_true(modified_verts, nodes[i].all_verts());
            },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }

      BKE_pbvh_sync_visibility_from_verts(object);
      pbvh.update_visibility(object);
      if (BKE_sculpt_multires_active(scene, &object)) {
        multires_mark_as_modified(depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
      }
      break;
    }
    case Type::HideFace: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object)) {
        return;
      }

      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      Array<bool> modified_faces(mesh.faces_num, false);
      for (std::unique_ptr<Node> &unode : step_data.nodes) {
        restore_hidden_face(object, *unode, modified_faces);
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              Vector<int> faces_vector;
              const Span<int> faces = bke::pbvh::node_face_indices_calc_grids(
                  subdiv_ccg, nodes[i], faces_vector);
              return indices_contain_true(modified_faces, faces);
            },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_faces, nodes[i].faces()); },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }

      hide::sync_all_from_faces(object);
      pbvh.update_visibility(object);
      break;
    }
    case Type::Mask: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object)) {
        return;
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        Array<bool> modified_grids(ss.subdiv_ccg->grids_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_mask_grids(object, *unode, modified_grids);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
            exec_mode::grain_size(1));
        bke::pbvh::update_mask_grids(*ss.subdiv_ccg, changed_nodes, pbvh);
        pbvh.tag_masks_changed(changed_nodes);
        multires_mark_as_modified(depsgraph, &object, MULTIRES_MASK_MODIFIED);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        Array<bool> modified_verts(mesh.verts_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_mask_mesh(object, *unode, modified_verts);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              return indices_contain_true(modified_verts, nodes[i].all_verts());
            },
            exec_mode::grain_size(1));
        bke::pbvh::update_mask_mesh(mesh, changed_nodes, pbvh);
        pbvh.tag_masks_changed(changed_nodes);
      }
      break;
    }
    case Type::FaceSet: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object)) {
        return;
      }

      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      Array<bool> modified_faces(mesh.faces_num, false);
      bool modified_face_set_grids = false;
      for (std::unique_ptr<Node> &unode : step_data.nodes) {
        restore_face_sets(object, *unode, modified_faces);
        if (use_multires_undo(step_data, ss)) {
          modified_face_set_grids |= restore_face_sets_grids(object, *unode);
        }
      }
      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const IndexMask changed_nodes = modified_face_set_grids ?
            node_mask :
            IndexMask::from_predicate(
                node_mask,
                memory,
                [&](const int i) {
                  Vector<int> faces_vector;
                  const Span<int> faces = bke::pbvh::node_face_indices_calc_grids(
                      subdiv_ccg, nodes[i], faces_vector);
                  return indices_contain_true(modified_faces, faces);
                },
                exec_mode::grain_size(1));
        pbvh.tag_face_sets_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_faces, nodes[i].faces()); },
            exec_mode::grain_size(1));
        pbvh.tag_face_sets_changed(changed_nodes);
      }
      break;
    }
    case Type::Color: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object)) {
        return;
      }

      if (pbvh.type() == bke::pbvh::Type::Grids) {
        restore_color_grids(object, step_data);
        /* Generic draw requests are invalidated by exact attribute name. An empty
         * name leaves the painted VBO unchanged even after the master is restored. */
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        if (mesh.active_color_attribute && mesh.active_color_attribute[0]) {
          pbvh.tag_attribute_changed(node_mask, mesh.active_color_attribute);
        }
        const char *trace = std::getenv("BLENDER_LAYER_RESP_TRACE");
        if (trace && trace[0] && trace[0] != '0') {
          std::printf("[POLYPAINT_UNDO] event=DRAW_INVALIDATE nodes=%lld attribute=%s\n",
                      static_cast<long long>(step_data.nodes.size()),
                      mesh.active_color_attribute ? mesh.active_color_attribute : "");
          std::fflush(stdout);
        }
        break;
      }

      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      Array<bool> modified_verts(mesh.verts_num, false);
      restore_color(object, step_data, modified_verts);
      const IndexMask changed_nodes = IndexMask::from_predicate(
          node_mask,
          memory,
          [&](const int i) { return indices_contain_true(modified_verts, nodes[i].all_verts()); },
          exec_mode::grain_size(1));
      pbvh.tag_attribute_changed(changed_nodes, mesh.active_color_attribute);
      break;
    }
    case Type::Geometry: {
      BLI_assert(!ss.bm);

      restore_geometry(step_data, object);
      BKE_sculptsession_free_deformMats(&ss);
      if (SubdivCCG *subdiv_ccg = ss.subdiv_ccg) {
        refine_subdiv(depsgraph, ss, object, subdiv_ccg->subdiv);
      }
      break;
    }
    case Type::DyntopoBegin:
    case Type::DyntopoEnd:
      /* Handled elsewhere. */
      BLI_assert_unreachable();
      break;
  }

  DEG_id_tag_update(&object.id, ID_RECALC_SHADING);
  if (tag_update) {
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
  }
}

static void free_step_data(StepData &step_data)
{
  geometry_free_data(&step_data.geometry_original);
  geometry_free_data(&step_data.geometry_modified);
  geometry_free_data(&step_data.bmesh.geometry_enter);
  if (step_data.bmesh.bm_entry) {
    BM_log_entry_drop(step_data.bmesh.bm_entry);
  }
  step_data.~StepData();
}

/**
 * Retrieve the undo data of a given type for the active undo step. For example, this is used to
 * access "original" data from before the current stroke.
 *
 * This is only possible when building an undo step, in between #push_begin and #push_end.
 */
static const Node *get_node(const bke::pbvh::Node *node, const Type type)
{
  StepData *step_data = get_step_data();
  if (!step_data) {
    return nullptr;
  }
  if (step_data->type != type) {
    return nullptr;
  }
  /* This access does not need to be locked because this function is not expected to be called
   * while the per-node undo data is being pushed. In other words, this must not be called
   * concurrently with #push_node. */
  std::unique_ptr<Node> *node_ptr = step_data->undo_nodes_by_pbvh_node.lookup_ptr(node);
  if (!node_ptr) {
    return nullptr;
  }
  return node_ptr->get();
}

static void store_vert_visibility_grids(const SubdivCCG &subdiv_ccg,
                                        const bke::pbvh::GridsNode &node,
                                        Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  if (grid_hidden.is_empty()) {
    return;
  }

  const Span<int> grid_indices = node.grids();
  unode.grid_hidden = BitGroupVector<0>(grid_indices.size(), grid_hidden.group_size());
  for (const int i : grid_indices.index_range()) {
    unode.grid_hidden[i].copy_from(grid_hidden[grid_indices[i]]);
  }
}

static void store_positions_mesh(const Depsgraph &depsgraph, const Object &object, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  gather_data_mesh(bke::pbvh::vert_positions_eval(depsgraph, object),
                   unode.vert_indices.as_span(),
                   unode.position.as_mutable_span());
  gather_data_mesh(bke::pbvh::vert_normals_eval(depsgraph, object),
                   unode.vert_indices.as_span(),
                   unode.normal.as_mutable_span());

  if (ss.deform_modifiers_active) {
    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    const Span<float3> orig_positions = ss.shapekey_active ? Span(static_cast<const float3 *>(
                                                                      ss.shapekey_active->data),
                                                                  mesh.verts_num) :
                                                             mesh.vert_positions();

    gather_data_mesh(
        orig_positions, unode.vert_indices.as_span(), unode.orig_position.as_mutable_span());
  }
}

static void store_positions_grids(const SubdivCCG &subdiv_ccg, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  gather_data_grids(
      subdiv_ccg, subdiv_ccg.positions.as_span(), unode.grids, unode.position.as_mutable_span());
  gather_data_grids(
      subdiv_ccg, subdiv_ccg.normals.as_span(), unode.grids, unode.normal.as_mutable_span());
}

static void store_vert_visibility_mesh(const Mesh &mesh, const bke::pbvh::Node &node, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_vert = *attributes.lookup<bool>(".hide_vert",
                                                              bke::AttrDomain::Point);
  if (hide_vert.is_empty()) {
    return;
  }

  const Span<int> verts = static_cast<const bke::pbvh::MeshNode &>(node).all_verts();
  for (const int i : verts.index_range()) {
    unode.vert_hidden[i].set(hide_vert[verts[i]]);
  }
}

static void store_face_visibility(const Mesh &mesh, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  if (hide_poly.is_empty()) {
    unode.face_hidden.fill(false);
    return;
  }
  const Span<int> faces = unode.face_indices;
  for (const int i : faces.index_range()) {
    unode.face_hidden[i].set(hide_poly[faces[i]]);
  }
}

static void store_mask_mesh(const Mesh &mesh, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  if (mask.is_empty()) {
    unode.mask.fill(0.0f);
  }
  else {
    gather_data_mesh(mask, unode.vert_indices.as_span(), unode.mask.as_mutable_span());
  }
}

static void store_mask_grids(const SubdivCCG &subdiv_ccg, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  if (!subdiv_ccg.masks.is_empty()) {
    gather_data_grids(
        subdiv_ccg, subdiv_ccg.masks.as_span(), unode.grids, unode.mask.as_mutable_span());
  }
  else {
    unode.mask.fill(0.0f);
  }
}

static void store_color(const Mesh &mesh, const bke::pbvh::MeshNode &node, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::GAttributeReader color_attribute = color::active_color_attribute(mesh);
  const GVArraySpan colors(*color_attribute);

  /* NOTE: even with loop colors we still store (derived)
   * vertex colors for original data lookup. */
  const Span<int> verts = node.verts();
  unode.col.reinitialize(verts.size());
  color::gather_colors_vert(
      faces, corner_verts, vert_to_face_map, colors, color_attribute.domain, verts, unode.col);

  if (color_attribute.domain == bke::AttrDomain::Corner) {
    for (const int face : node.faces()) {
      for (const int corner : faces[face]) {
        unode.corner_indices.append(corner);
      }
    }
    unode.loop_col.reinitialize(unode.corner_indices.size());
    color::gather_colors(colors, unode.corner_indices, unode.loop_col);
  }
}

static NodeGeometry *geometry_get(StepData &step_data)
{
  if (!step_data.geometry_original.is_initialized) {
    return &step_data.geometry_original;
  }

  BLI_assert(!step_data.geometry_modified.is_initialized);

  return &step_data.geometry_modified;
}

static void geometry_push(const Object &object)
{
  StepData *step_data = get_step_data();

  step_data->type = Type::Geometry;

  NodeGeometry *geometry = geometry_get(*step_data);
  store_geometry_data(geometry, object);
}

static void store_face_sets(const Mesh &mesh, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
  if (face_sets.is_empty()) {
    unode.face_sets.fill(1);
  }
  else {
    gather_data_mesh(face_sets, unode.face_indices.as_span(), unode.face_sets.as_mutable_span());
  }
}

static void fill_node_data_mesh(const Depsgraph &depsgraph,
                                const Object &object,
                                const bke::pbvh::MeshNode &node,
                                const Type type,
                                Node &unode)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Mesh &mesh = *id_cast<Mesh *>(object.data);

  unode.vert_indices = node.all_verts();
  unode.unique_verts_num = node.verts().size();

  const int verts_num = unode.vert_indices.size();

  if (ELEM(type, Type::FaceSet, Type::HideFace)) {
    unode.face_indices = node.faces();
  }

  switch (type) {
    case Type::None:
      BLI_assert_unreachable();
      break;
    case Type::Position: {
      unode.position.reinitialize(verts_num);
      /* Needed for original data lookup. */
      unode.normal.reinitialize(verts_num);
      if (ss.deform_modifiers_active) {
        unode.orig_position.reinitialize(verts_num);
      }
      store_positions_mesh(depsgraph, object, unode);
      break;
    }
    case Type::HideVert: {
      unode.vert_hidden.resize(unode.vert_indices.size());
      store_vert_visibility_mesh(mesh, node, unode);
      break;
    }
    case Type::HideFace: {
      unode.face_hidden.resize(unode.face_indices.size());
      store_face_visibility(mesh, unode);
      break;
    }
    case Type::Mask: {
      unode.mask.reinitialize(verts_num);
      store_mask_mesh(mesh, unode);
      break;
    }
    case Type::Color: {
      store_color(mesh, node, unode);
      break;
    }
    case Type::DyntopoBegin:
    case Type::DyntopoEnd:
      /* Dyntopo should be handled elsewhere. */
      BLI_assert_unreachable();
      break;
    case Type::Geometry:
      /* See #geometry_push. */
      BLI_assert_unreachable();
      break;
    case Type::FaceSet: {
      unode.face_sets.reinitialize(unode.face_indices.size());
      store_face_sets(mesh, unode);
      break;
    }
  }
}

static void store_sculpt_layer_disps(const Object &object,
                                     Node &unode,
                                     const bool persistent_snapshot_valid)
{
  if (sculpt_layer_undo_ccg_enabled() || !sculpt_layer_undo_persistent_enabled() ||
      !persistent_snapshot_valid)
  {
    return;
  }
  const SculptSession &ss = *object.runtime->sculpt_session;
  if (!ss.multires_modifier) {
    return;
  }
  const MultiresModifierData &mmd = *ss.multires_modifier;
  if (mmd.totlvl <= 0 || !bke::multires_layers::base_has_visible_layers(mmd)) {
    return;
  }
  const MultiresSculptLayer *layer = bke::multires_layers::active(mmd);
  if (!layer || !layer->disps || layer->level != mmd.totlvl ||
      layer->grids_num != id_cast<const Mesh *>(object.data)->corners_num)
  {
    return;
  }

  const int side = bke::subdiv::grid_size_from_level(layer->level);
  const int area = side * side;
  if (side < 2 || area <= 0) {
    return;
  }

  /* Validate every descriptor before allocating the undo payload. Sparse null payloads are valid
   * and represent an implicit all-zero grid. */
  for (const int grid : unode.grids) {
    if (grid < 0 || grid >= layer->grids_num || layer->disps[grid].level != layer->level ||
        layer->disps[grid].totdisp != area)
    {
      return;
    }
  }

  unode.sculpt_layer_grid_area = area;
  unode.sculpt_layer_disps.reinitialize(int64_t(unode.grids.size()) * int64_t(area));
  for (const int grid_i : unode.grids.index_range()) {
    const int grid = unode.grids[grid_i];
    MutableSpan<float3> dst = unode.sculpt_layer_disps.as_mutable_span().slice(
        int64_t(grid_i) * area, area);
    const float (*src)[3] = layer->disps[grid].disps;
    if (src == nullptr) {
      dst.fill(float3(0.0f, 0.0f, 0.0f));
      continue;
    }
    for (const int i : dst.index_range()) {
      dst[i] = float3(src[i]);
    }
  }
}

static void fill_node_data_grids(const Object &object,
                                 const bke::pbvh::GridsNode &node,
                                 const Type type,
                                 Node &unode,
                                 const bool persistent_snapshot_valid)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  unode.grids = node.grids();

  const int grid_area = subdiv_ccg.grid_size * subdiv_ccg.grid_size;
  const int verts_num = unode.grids.size() * grid_area;

  if (ELEM(type, Type::FaceSet, Type::HideFace)) {
    bke::pbvh::node_face_indices_calc_grids(subdiv_ccg, node, unode.face_indices);
  }

  switch (type) {
    case Type::None:
      BLI_assert_unreachable();
      break;
    case Type::Position: {
      unode.position.reinitialize(verts_num);
      /* Needed for original data lookup. */
      unode.normal.reinitialize(verts_num);
      store_sculpt_layer_disps(object, unode, persistent_snapshot_valid);
      store_positions_grids(subdiv_ccg, unode);
      break;
    }
    case Type::HideVert: {
      store_vert_visibility_grids(subdiv_ccg, node, unode);
      break;
    }
    case Type::HideFace: {
      unode.face_hidden.resize(unode.face_indices.size());
      store_face_visibility(base_mesh, unode);
      break;
    }
    case Type::Mask: {
      unode.mask.reinitialize(verts_num);
      store_mask_grids(subdiv_ccg, unode);
      break;
    }
    case Type::Color: {
      MultiresModifierData *mmd = ss.multires_modifier;
      /* The first color stroke may materialize a previously implicit grid.
       * Capture presence separately, so Undo returns to the base attribute. */
      if (!mmd || mmd->totlvl < 1 || mmd->totlvl > 13) {
        break;
      }
      if (mmd->sculpt_color_grids &&
          mmd->sculpt_color_grids_num != subdiv_ccg.grids_num)
      {
        break;
      }
      if (mmd->sculpt_color_grids && mmd->sculpt_color_level < mmd->totlvl &&
          !bke::multires_layers::polypaint_resample(*mmd, mmd->totlvl))
      {
        break;
      }
      const int master_level = mmd->sculpt_color_grids ? mmd->sculpt_color_level :
                                                             mmd->totlvl;
      unode.grid_color_side = bke::subdiv::grid_size_from_level(master_level);
      const int master_area = unode.grid_color_side * unode.grid_color_side;
      unode.col.reinitialize(unode.grids.size() * master_area);
      unode.col.fill(float4(0.0f));
      unode.grid_color_present.resize(unode.grids.size());
      for (const int i : unode.grids.index_range()) {
        const float (*colors)[4] = bke::multires_layers::polypaint_grid(*mmd, unode.grids[i]);
        if (!colors) {
          unode.grid_color_present[i].set(false);
          continue;
        }
        unode.grid_color_present[i].set(true);
        for (int p = 0; p < master_area; p++) {
          unode.col[i * master_area + p] = float4(colors[p]);
        }
      }
      break;
    }
    case Type::DyntopoBegin:
    case Type::DyntopoEnd:
      /* Dyntopo should be handled elsewhere. */
      BLI_assert_unreachable();
      break;
    case Type::Geometry:
      /* See #geometry_push. */
      BLI_assert_unreachable();
      break;
    case Type::FaceSet: {
      unode.face_sets.reinitialize(unode.face_indices.size());
      store_face_sets(base_mesh, unode);

      MultiresModifierData *mmd = ss.multires_modifier;
      if (mmd && mmd->totlvl > 0 &&
          bke::multires_layers::faceset_ensure_from_mesh(*mmd, base_mesh, mmd->totlvl))
      {
        unode.grid_face_set_side = bke::subdiv::grid_size_from_level(mmd->sculpt_face_set_level);
        const int master_area = unode.grid_face_set_side * unode.grid_face_set_side;
        unode.grid_face_sets.reinitialize(unode.grids.size() * master_area);

        int offset = 0;
        for (const int grid : unode.grids) {
          const int *ids = bke::multires_layers::faceset_grid(*mmd, grid);
          const MultiresFaceSetGrid &stored = mmd->sculpt_face_set_grids[grid];
          if (ids) {
            std::copy_n(ids, master_area, unode.grid_face_sets.data() + offset);
          }
          else {
            std::fill_n(unode.grid_face_sets.data() + offset, master_area, stored.uniform_id);
          }
          offset += master_area;
        }
      }
      break;
    }
  }
}

/**
 * Dynamic topology stores only one undo node per stroke, regardless of the number of
 * bke::pbvh::Tree nodes modified.
 */
BLI_NOINLINE static void bmesh_push(const Object &object,
                                    const bke::pbvh::BMeshNode *node,
                                    Type type)
{
  StepData *step_data = get_step_data();
  const SculptSession &ss = *object.runtime->sculpt_session;

  std::scoped_lock lock(step_data->nodes_mutex);

  if (step_data->nodes.is_empty()) {
    /* We currently need to append data here so that the overall undo system knows to indicate that
     * data should be flushed to the memfile */
    /* TODO: Once we store entering Sculpt Mode as a specific type of action, we can remove this
     * call. */
    step_data->nodes.append(std::make_unique<Node>());

    step_data->type = type;

    if (type == Type::DyntopoEnd) {
      step_data->bmesh.bm_entry = BM_log_entry_add(ss.bm_log);
      BM_log_before_all_removed(ss.bm, ss.bm_log);
    }
    else if (type == Type::DyntopoBegin) {
      /* Store a copy of the mesh's current vertices, loops, and
       * faces. A full copy like this is needed because entering
       * dynamic-topology immediately does topological edits
       * (converting faces to triangles) that the BMLog can't
       * fully restore from. */
      NodeGeometry *geometry = &step_data->bmesh.geometry_enter;
      store_geometry_data(geometry, object);

      step_data->bmesh.bm_entry = BM_log_entry_add(ss.bm_log);
      BM_log_all_added(ss.bm, ss.bm_log);
    }
    else {
      step_data->bmesh.bm_entry = BM_log_entry_add(ss.bm_log);
    }
  }

  if (node) {
    const int cd_vert_mask_offset = CustomData_get_offset_named(
        &ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");

    /* The vertices and node aren't changed, though pointers to them are stored in the log. */
    bke::pbvh::BMeshNode *node_mut = const_cast<bke::pbvh::BMeshNode *>(node);

    switch (type) {
      case Type::None:
        BLI_assert_unreachable();
        break;
      case Type::Position:
      case Type::Mask:
        /* Before any vertex values get modified, ensure their
         * original positions are logged. */
        for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }
        for (BMVert *vert : BKE_pbvh_bmesh_node_other_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }
        break;

      case Type::HideFace:
      case Type::HideVert: {
        for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }
        for (BMVert *vert : BKE_pbvh_bmesh_node_other_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }

        for (BMFace *f : BKE_pbvh_bmesh_node_faces(node_mut)) {
          BM_log_face_modified(ss.bm_log, f);
        }
        break;
      }

      case Type::DyntopoBegin:
      case Type::DyntopoEnd:
      case Type::Geometry:
      case Type::FaceSet:
      case Type::Color:
        break;
    }
  }
}

/**
 * Add an undo node for the bke::pbvh::Tree node to the step's storage. If the node was
 * newly created and needs to be filled with data, set \a r_new to true.
 */
static Node *ensure_node(StepData &step_data, const bke::pbvh::Node &node, bool &r_new)
{
  std::scoped_lock lock(step_data.nodes_mutex);
  r_new = false;
  std::unique_ptr<Node> &unode = step_data.undo_nodes_by_pbvh_node.lookup_or_add_cb(&node, [&]() {
    std::unique_ptr<Node> new_unode = std::make_unique<Node>();
    r_new = true;
    return new_unode;
  });
  return unode.get();
}

void push_node(const Depsgraph &depsgraph,
               const Object &object,
               const bke::pbvh::Node *node,
               const Type type)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (ss.bm || ELEM(type, Type::DyntopoBegin, Type::DyntopoEnd)) {
    bmesh_push(object, static_cast<const bke::pbvh::BMeshNode *>(node), type);
    return;
  }

  StepData *step_data = get_step_data();
  BLI_assert(ELEM(step_data->type, Type::None, type));
  step_data->type = type;

  bool newly_added;
  Node *unode = ensure_node(*step_data, *node, newly_added);
  if (!newly_added) {
    /* The node was already filled with data for this undo step. */
    return;
  }

  ss.needs_flush_to_id = true;

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      fill_node_data_mesh(
          depsgraph, object, static_cast<const bke::pbvh::MeshNode &>(*node), type, *unode);
      break;
    case bke::pbvh::Type::Grids:
      fill_node_data_grids(object,
                           static_cast<const bke::pbvh::GridsNode &>(*node),
                           type,
                           *unode,
                           step_data->layer_persistent_snapshot_valid);
      break;
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      break;
  }
}

void push_nodes(const Depsgraph &depsgraph,
                Object &object,
                const IndexMask &node_mask,
                const Type type)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;

  ss.needs_flush_to_id = true;

  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (ss.bm || ELEM(type, Type::DyntopoBegin, Type::DyntopoEnd)) {
    const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
    node_mask.foreach_index([&](const int i) { bmesh_push(object, &nodes[i], type); });
    return;
  }

  StepData *step_data = get_step_data();
  BLI_assert(ELEM(step_data->type, Type::None, type));
  step_data->type = type;

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      Vector<std::pair<const bke::pbvh::MeshNode *, Node *>, 32> nodes_to_fill;
      node_mask.foreach_index([&](const int i) {
        bool newly_added;
        Node *unode = ensure_node(*step_data, nodes[i], newly_added);
        if (newly_added) {
          nodes_to_fill.append({&nodes[i], unode});
        }
      });
      threading::parallel_for(nodes_to_fill.index_range(), 1, [&](const IndexRange range) {
        for (const auto &[node, unode] : nodes_to_fill.as_span().slice(range)) {
          fill_node_data_mesh(depsgraph, object, *node, type, *unode);
        }
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      Vector<std::pair<const bke::pbvh::GridsNode *, Node *>, 32> nodes_to_fill;
      node_mask.foreach_index([&](const int i) {
        bool newly_added;
        Node *unode = ensure_node(*step_data, nodes[i], newly_added);
        if (newly_added) {
          nodes_to_fill.append({&nodes[i], unode});
        }
      });
      threading::parallel_for(nodes_to_fill.index_range(), 1, [&](const IndexRange range) {
        for (const auto &[node, unode] : nodes_to_fill.as_span().slice(range)) {
          fill_node_data_grids(
              object, *node, type, *unode, step_data->layer_persistent_snapshot_valid);
        }
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BLI_assert_unreachable();
      break;
    }
  }
}

static void save_active_attribute(Object &object, SculptAttrRef *attr)
{
  Mesh *mesh = BKE_object_get_original_mesh(&object);
  attr->was_set = true;
  attr->domain = NO_ACTIVE_LAYER;
  attr->name[0] = 0;
  if (!mesh) {
    return;
  }
  const char *name = mesh->active_color_attribute;
  const bke::AttributeAccessor attributes = mesh->attributes();
  const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(name);
  if (!bke::mesh::is_color_attribute(meta_data)) {
    return;
  }
  attr->domain = meta_data->domain;
  STRNCPY_UTF8(attr->name, name);
  attr->type = *bke::attr_type_to_custom_data_type(meta_data->data_type);
}

/**
 * Does not save topology counts, as that data is unneeded for full geometry pushes and
 * requires the PBVH to exist.
 */
static void save_common_data(Object &ob, SculptUndoStep *us)
{
  us->data.object_name = ob.id.name;
  if (ob.runtime->sculpt_session && ob.runtime->sculpt_session->subdiv_ccg &&
      ob.runtime->sculpt_session->multires_modifier &&
      (sculpt_layer_undo_ccg_enabled() || sculpt_layer_undo_frame_enabled() ||
       sculpt_layer_undo_persistent_enabled() ||
       !bke::multires_layers::base_has_visible_layers(
           *ob.runtime->sculpt_session->multires_modifier)))
  {
    for (ModifierData &md : ob.modifiers) {
      if (md.type != eModifierType_Multires ||
          &md != &ob.runtime->sculpt_session->multires_modifier->modifier)
      {
        continue;
      }
      const auto &mmd = *reinterpret_cast<MultiresModifierData *>(&md);
      if (mmd.sculpt_layers.is_empty()) {
        break;
      }
      us->data.native_multires_base_undo =
          !bke::multires_layers::base_has_visible_layers(mmd);
      us->data.layer_frame_modifier = md.name;
      us->data.layer_frame_active = mmd.sculpt_layer_active;
      us->data.layer_frame_level = mmd.sculptlvl;
      us->data.layer_frame_top_level = mmd.totlvl;
      const SubdivCCG &ccg = *ob.runtime->sculpt_session->subdiv_ccg;
      us->data.layer_persistent_snapshot_valid =
          !sculpt_layer_undo_ccg_enabled() && !us->data.native_multires_base_undo &&
          mmd.sculptlvl == mmd.totlvl &&
          ccg.level == mmd.totlvl && !ccg.dirty.coords && !ccg.dirty.hidden &&
          !ccg.dirty.masks;
      for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
        us->data.layer_frame.append({layer.name, layer.strength,
                                    (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0});
      }
      break;
    }
  }


  if (sculpt_layer_undo_metadata_enabled()) {
    for (ModifierData &md : ob.modifiers) {
      if (md.type != eModifierType_Multires) {
        continue;
      }
      const auto &mmd = reinterpret_cast<const MultiresModifierData &>(md);
      if (mmd.sculpt_layers.is_empty()) {
        continue;
      }
      us->data.layer_metadata_modifier = md.name;
      for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
        us->data.layer_metadata.append({layer.name, layer.strength,
                                     (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0});
      }
      break;
    }
  }



  if (!us->active_color_start.was_set) {
    save_active_attribute(ob, &us->active_color_start);
  }

  /* Set end attribute in case push_end is not called,
   * so we don't end up with corrupted state.
   */
  if (!us->active_color_end.was_set) {
    save_active_attribute(ob, &us->active_color_end);
    us->active_color_end.was_set = false;
  }

  const SculptSession &ss = *ob.runtime->sculpt_session;

  us->data.pivot_pos = ss.pivot_pos;
  us->data.pivot_rot = ss.pivot_rot;

  if (const KeyBlock *key = BKE_keyblock_from_object(&ob)) {
    us->data.active_shape_key_name = key->name;
  }
}

/* SCULPT_LAYER_FIRST_STROKE_ANCHOR_V41
 * Sculpt decodes the current step when undoing. If its preceding visible state
 * is a Memfile, even a successful local position swap is followed by a full
 * Memfile decode. Create a native Type::None boundary BEFORE any stroke edits,
 * using the same history name as the post-Add or Set Active Modifier checkpoint. Keep the checkpoint
 * as an internal step, so undoing Add and redoing it still read all scene IDs.
 * No global snapshot, high-resolution copy, or flush is created here. */
static bool sculpt_layer_first_stroke_anchor_eligible(const UndoStack &ustack,
                                                       const Object &ob,
                                                       const char *&r_reason)
{
  const UndoStep *checkpoint = ustack.step_active;
  if (ustack.step_init) {
    r_reason = "ENCODE_IN_PROGRESS";
    return false;
  }
  if (ustack.group_level != 0) {
    r_reason = "UNDO_GROUP";
    return false;
  }
  if (!checkpoint || checkpoint->type != BKE_UNDOSYS_TYPE_MEMFILE || checkpoint->skip ||
      !checkpoint->prev)
  {
    r_reason = "HISTORY_STATE";
    return false;
  }
  if (checkpoint != ustack.step_active_memfile) {
    r_reason = "CHECKPOINT_NOT_ACTIVE";
    return false;
  }
  if (std::string(checkpoint->name) != "Add Sculpt Layer" &&
      std::string(checkpoint->name) != "Set Active Modifier")
  {
    r_reason = "CHECKPOINT_NAME";
    return false;
  }
  /* A new stroke already discards redo via push_init_with_type. Let that same
   * function discard it when inserting the boundary; the active checkpoint is
   * retained. Rejecting checkpoint->next prevented this after a first Undo. */
  if (ob.type != OB_MESH || !(ob.mode & OB_MODE_SCULPT) || ob.id.lib || !ob.data ||
      ID_REAL_USERS(ob.data) != 1 || (ob.id.recalc & ID_RECALC_GEOMETRY))
  {
    r_reason = "OBJECT";
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const SculptSession *ss = ob.runtime->sculpt_session;
  const SubdivCCG *ccg = ss ? ss->subdiv_ccg : nullptr;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (mesh.id.lib || !ss || ss->bm || ss->filter_cache || ss->expand_cache ||
      !ccg || !pbvh || pbvh->type() != bke::pbvh::Type::Grids ||
      ccg->dirty.coords || ccg->dirty.hidden || ccg->dirty.masks ||
      ss->deform_modifiers_active || ss->shapekey_active)
  {
    r_reason = "SCULPT_STATE";
    return false;
  }
  const MultiresModifierData *mmd = nullptr;
  for (const ModifierData &md : ob.modifiers) {
    if (!(md.mode & eModifierMode_Realtime) || (md.mode & eModifierMode_DisableTemporary)) {
      continue;
    }
    if (md.type != eModifierType_Multires || mmd) {
      r_reason = "MODIFIER_STACK";
      return false;
    }
    mmd = reinterpret_cast<const MultiresModifierData *>(&md);
  }
  if (!mmd || mmd != ss->multires_modifier || mmd->totlvl <= 0 ||
      mmd->sculptlvl != mmd->totlvl || ccg->level != mmd->totlvl ||
      ccg->grids_num != mesh.corners_num ||
      ccg->grid_size != bke::subdiv::grid_size_from_level(mmd->totlvl) ||
      ccg->grid_area != ccg->grid_size * ccg->grid_size ||
      ccg->positions.size() != int64_t(ccg->grids_num) * ccg->grid_area ||
      mmd->sculpt_displacement_enabled || mmd->sculpt_shrinkwrap_enabled ||
      mmd->sculpt_layers.is_empty() ||
      mmd->sculpt_layer_active != mmd->sculpt_layers.count() - 1)
  {
    r_reason = "MULTIRES_FRAME";
    return false;
  }
  const MultiresSculptLayer *layer = bke::multires_layers::active(*mmd);
  if (!layer || !bke::multires_layers::is_effective(*layer) || layer->base_deltas ||
      layer->base_verts_num != 0 || layer->level != mmd->totlvl || !layer->disps ||
      layer->grids_num != ccg->grids_num)
  {
    r_reason = "NEW_LAYER";
    return false;
  }
  /* A newly added sparse layer has no payload. Check descriptors only; never
   * scan or allocate high-resolution samples in the pre-stroke path. */
  for (int grid = 0; grid < layer->grids_num; grid++) {
    if (layer->disps[grid].disps || layer->disps[grid].level != mmd->totlvl ||
        layer->disps[grid].totdisp != ccg->grid_area)
    {
      r_reason = "LAYER_NOT_EMPTY_SPARSE";
      return false;
    }
  }
  r_reason = "READY";
  return true;
}

static void sculpt_layer_first_stroke_anchor_ensure(UndoStack &ustack, Object &ob)
{
  const char *env = std::getenv("BLENDER_SCULPT_FIRST_STROKE_ANCHOR");
  if (!env || env[0] != '1' || env[1] != '\0') {
    return;
  }
  const double anchor_start = BLI_time_now_seconds();
  const char *reason = nullptr;
  if (!sculpt_layer_first_stroke_anchor_eligible(ustack, ob, reason)) {
    if (ustack.step_active && ustack.step_active->type == BKE_UNDOSYS_TYPE_MEMFILE &&
        (std::string(ustack.step_active->name) == "Add Sculpt Layer" ||
         std::string(ustack.step_active->name) == "Set Active Modifier"))
    {
      std::printf("[LAYER_UNDO_ANCHOR] mode=SKIP reason=%s\n", reason);
      std::fflush(stdout);
    }
    return;
  }
  UndoStep *checkpoint = ustack.step_active;
  SculptUndoStep *anchor = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(
          &ustack, nullptr, checkpoint->name, BKE_UNDOSYS_TYPE_SCULPT));
  if (!anchor) {
    return;
  }
  save_common_data(ob, anchor);
  save_active_attribute(ob, &anchor->active_color_end);
  anchor->data.grids.grids_num = ob.runtime->sculpt_session->subdiv_ccg->grids_num;
  anchor->data.grids.grid_size = ob.runtime->sculpt_session->subdiv_ccg->grid_size;
  anchor->data.layer_add_boundary_anchor = true;

  const eUndoPushReturn result = BKE_undosys_step_push(&ustack, nullptr, nullptr);
  if ((result & UNDO_PUSH_RET_SUCCESS) && ustack.step_active == &anchor->step &&
      anchor->step.prev == checkpoint && ustack.step_active_memfile == checkpoint)
  {
    /* Replace the visible Add state, not the global checkpoint itself. The
     * first stroke now undoes onto this native boundary. Crossing back before
     * Add still decodes the retained Memfile and the earlier scene checkpoint. */
    checkpoint->skip = true;
    std::printf("[LAYER_UNDO_ANCHOR] mode=CREATE version=4.1 checkpoint=%s grids=%d total=%.3fms\n",
                checkpoint->name, anchor->data.grids.grids_num,
                (BLI_time_now_seconds() - anchor_start) * 1000.0);
    std::fflush(stdout);
  }
}

void push_begin_ex(const Scene & /*scene*/, Object &ob, const char *name)
{
  UndoStack *ustack = ED_undo_stack_get();
  sculpt_layer_first_stroke_anchor_ensure(*ustack, ob);

  /* If possible, we need to tag the object and its geometry data as 'changed in the future' in
   * the previous undo step if it's a memfile one. */
  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  /* Special case, we never read from this. */
  bContext *C = nullptr;

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_SCULPT));

  const SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  save_common_data(ob, us);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
      us->data.mesh.verts_num = mesh.verts_num;
      us->data.mesh.corners_num = mesh.corners_num;
      if (MultiresSculptLayer *layer = bke::multires_layers::base_recording_layer(ob)) {
        if (bke::multires_layers::base_deltas_ensure(*layer, mesh.verts_num)) {
          us->data.base_modifier_name = bke::multires_layers::base_modifier(ob)->modifier.name;
          us->data.base_layer_name = layer->name;
          us->data.base_layer_deltas = Array<float3>(
              Span(reinterpret_cast<const float3 *>(layer->base_deltas), layer->base_verts_num));
        }
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      us->data.grids.grids_num = ss.subdiv_ccg->grids_num;
      us->data.grids.grid_size = ss.subdiv_ccg->grid_size;
      break;
    }
    case bke::pbvh::Type::BMesh: {
      break;
    }
  }
}

void push_begin(const Scene &scene, Object &ob, const wmOperator *op)
{
  push_begin_ex(scene, ob, op->type->name);
}

void push_enter_sculpt_mode(const Scene & /*scene*/, Object &ob, const wmOperator *op)
{
  UndoStack *ustack = ED_undo_stack_get();

  /* If possible, we need to tag the object and its geometry data as 'changed in the future' in
   * the previous undo step if it's a memfile one. */
  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  /* Special case, we never read from this. */
  bContext *C = nullptr;

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(ustack, C, op->type->name, BKE_UNDOSYS_TYPE_SCULPT));
  save_common_data(ob, us);
}

static size_t node_size_in_bytes(const Node &node)
{
  size_t size = sizeof(Node);
  size += node.position.as_span().size_in_bytes();
  size += node.orig_position.as_span().size_in_bytes();
  size += node.normal.as_span().size_in_bytes();
  size += node.col.as_span().size_in_bytes();
  size += node.mask.as_span().size_in_bytes();
  size += node.loop_col.as_span().size_in_bytes();
  size += node.vert_indices.as_span().size_in_bytes();
  size += node.corner_indices.as_span().size_in_bytes();
  size += node.vert_hidden.size() / 8;
  size += node.face_hidden.size() / 8;
  size += node.grids.as_span().size_in_bytes();
  size += node.grid_hidden.all_bits().size() / 8;
  size += node.face_sets.as_span().size_in_bytes();
  size += node.face_indices.as_span().size_in_bytes();
  return size;
}

void push_end_ex(Object &ob, const bool use_nested_undo)
{
  StepData *step_data = get_step_data();

  /* Move undo node storage from map to vector. */
  step_data->nodes.reserve(step_data->undo_nodes_by_pbvh_node.size());
  for (std::unique_ptr<Node> &node : step_data->undo_nodes_by_pbvh_node.values()) {
    step_data->nodes.append(std::move(node));
  }
  step_data->undo_nodes_by_pbvh_node.clear();

  /* We don't need normals in the undo stack. */
  for (std::unique_ptr<Node> &unode : step_data->nodes) {
    unode->normal = {};
  }
  /* TODO: When #Node.orig_positions is stored, #Node.positions is unnecessary, don't keep it in
   * the stored undo step. In the future the stored undo step should use a different format with
   * just one positions array that has a different semantic meaning depending on whether there are
   * deform modifiers. */

  if (step_data->type == Type::Position) {
    step_data->position_step_storage = std::make_unique<PositionUndoStorage>(*step_data);
  }
  else {
    step_data->undo_size = threading::parallel_reduce(
        step_data->nodes.index_range(),
        16,
        0,
        [&](const IndexRange range, size_t size) {
          for (const int i : range) {
            size += node_size_in_bytes(*step_data->nodes[i]);
          }
          return size;
        },
        std::plus<size_t>());
  }

  /* We could remove this and enforce all callers run in an operator using 'OPTYPE_UNDO'. */
  wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
  if (wm->op_undo_depth == 0 || use_nested_undo) {
    UndoStack *ustack = ED_undo_stack_get();
    BKE_undosys_step_push(ustack, nullptr, nullptr);
    if (wm->op_undo_depth == 0) {
      BKE_undosys_stack_limit_steps_and_memory_defaults(ustack);
    }
    WM_file_tag_modified();
  }

  UndoStack *ustack = ED_undo_stack_get();
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_stack_init_or_active_with_type(ustack, BKE_UNDOSYS_TYPE_SCULPT));

  save_active_attribute(ob, &us->active_color_end);
}

void push_end(Object &ob)
{
  push_end_ex(ob, false);
}

/* -------------------------------------------------------------------- */
/** \name Implements ED Undo System
 * \{ */

static void set_active_layer(bContext *C, const SculptAttrRef *attr_ref)
{
  if (attr_ref->domain == bke::AttrDomain::Auto) {
    return;
  }

  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_object_get_original_mesh(ob);

  SculptAttrRef existing;
  save_active_attribute(*ob, &existing);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();

  /* Temporary fix for #97408. This is a fundamental
   * bug in the undo stack; the operator code needs to push
   * an extra undo step before running an operator if a
   * non-memfile undo system is active.
   *
   * For now, detect if the layer does exist but with a different
   * domain and just unconvert it.
   */
  if (const bke::GAttributeReader attr = attributes.lookup(attr_ref->name)) {
    if (attr.domain != attr_ref->domain ||
        bke::cpp_type_to_custom_data_type(attr.varray.type()) != attr_ref->type)
    {
      AttributeOwner owner = AttributeOwner::from_id(&mesh->id);
      if (ed::geometry::convert_attribute(owner,
                                          mesh->attributes_for_write(),
                                          attr_ref->name,
                                          attr_ref->domain,
                                          *bke::custom_data_type_to_attr_type(attr_ref->type),
                                          nullptr))
      {
      }
    }
  }

  if (!attributes.contains(attr_ref->name)) {
    /* Memfile undo killed the layer; re-create it. */
    mesh->attributes_for_write().add(attr_ref->name,
                                     attr_ref->domain,
                                     *bke::custom_data_type_to_attr_type(attr_ref->type),
                                     bke::AttributeInitDefaultValue());
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }

  if (attributes.contains(attr_ref->name)) {
    BKE_id_attributes_active_color_set(&mesh->id, attr_ref->name);
  }
}

static void step_encode_init(bContext * /*C*/, UndoStep *us_p)
{
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);
  new (&us->data) StepData();
}

static bool step_encode(bContext * /*C*/, Main *bmain, UndoStep *us_p)
{
  /* Dummy, encoding is done along the way by adding tiles
   * to the current 'SculptUndoStep' added by encode_init. */
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);
  us->step.data_size = us->data.undo_size + us->data.base_layer_deltas.size() * sizeof(float3);
  if (us->data.type == Type::Color && us->data.grids.grids_num != 0) {
    const char *trace = std::getenv("BLENDER_LAYER_RESP_TRACE");
    if (trace && trace[0] && trace[0] != '0') {
      std::printf("[POLYPAINT_UNDO] event=ENCODE step=%s nodes=%lld grids=%d bytes=%llu\n",
                  us_p->name, static_cast<long long>(us->data.nodes.size()),
                  us->data.grids.grids_num,
                  static_cast<unsigned long long>(us->data.undo_size));
      std::fflush(stdout);
    }
  }
  us->step.data_size += us->data.layer_frame.size() * sizeof(SculptLayerUndoFrameItem);
  us->step.data_size += us->data.layer_frame_modifier.size();
  for (const SculptLayerUndoFrameItem &item : us->data.layer_frame) {
    us->step.data_size += item.name.size();
  }
  us->step.data_size += us->data.layer_metadata.size() * sizeof(SculptLayerUndoMetadata);
  us->step.data_size += us->data.layer_metadata_modifier.size();
  for (const SculptLayerUndoMetadata &saved : us->data.layer_metadata) {
    us->step.data_size += saved.name.size();
  }

  if (us->data.type == Type::DyntopoEnd) {
    us->step.use_memfile_step = true;
  }
  us->step.is_applied = true;

  /* We do not flush data when entering sculpt mode - this is currently indicated by Type::None */
  if (us->data.type != Type::None) {
    bmain->is_memfile_undo_flush_needed = true;
  }

  return true;
}

/* Opt-in sampled state trace. It observes live state and never commits or edits data. */
static void sculpt_layer_undo_trace_state(bContext *C,
                                          const SculptUndoStep &us,
                                          const char *event)
{
  const char *trace = std::getenv("BLENDER_SCULPT_LAYER_UNDO_TRACE");
  if (!trace || trace[0] != '1') {
    return;
  }
  const Object *object = CTX_data_active_object(C);
  if (!object || object->type != OB_MESH) {
    return;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object->data);
  const SculptSession *ss = object->runtime->sculpt_session;
  const SubdivCCG *ccg = ss ? ss->subdiv_ccg : nullptr;
  std::printf(
      "[UNDO_STATE] event=%s step=%s type=%d mode=%d mesh=%d/%d/%d "
      "saved_grids=%d saved_side=%d live_grids=%d live_side=%d pending=%d/%d/%d\n",
      event, us.step.name, int(us.data.type), int(object->mode),
      mesh.verts_num, mesh.faces_num, mesh.corners_num,
      us.data.grids.grids_num, us.data.grids.grid_size,
      ccg ? ccg->grids_num : 0, ccg ? ccg->grid_size : 0,
      ccg ? int(ccg->dirty.coords) : 0,
      ccg ? int(ccg->dirty.hidden) : 0,
      ccg ? int(ccg->dirty.masks) : 0);
  for (const ModifierData &md : object->modifiers) {
    if (md.type != eModifierType_Multires) {
      continue;
    }
    const auto &mmd = reinterpret_cast<const MultiresModifierData &>(md);
    int layer_index = 0;
    for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
      /* Bound the cost to 256 grids and three samples per grid. This is a sampled
       * signature: zero energy does not prove the complete layer is empty. */
      double energy = 0.0;
      double signature = 0.0;
      int samples = 0;
      const int grids = std::min(layer.grids_num, 256);
      for (int sample_grid = 0; layer.disps && sample_grid < grids; sample_grid++) {
        const int corner = int(int64_t(sample_grid) * layer.grids_num / grids);
        const MDisps &grid = layer.disps[corner];
        if (!grid.disps || grid.totdisp <= 0) {
          continue;
        }
        const int indices[3] = {0, grid.totdisp / 2, grid.totdisp - 1};
        for (const int index : indices) {
          for (int axis = 0; axis < 3; axis++) {
            const double value = grid.disps[index][axis];
            energy += value * value;
            signature += value * double(1 + corner * 3 + axis);
          }
          samples++;
        }
      }
      std::printf(
          "[UNDO_LAYER_STATE] event=%s modifier=%s sculpt=%d top=%d layer=%d name=%s "
          "visible=%d strength=%.9g grids=%d sampled=%d energy=%.17g signature=%.17g\n",
          event, md.name, mmd.sculptlvl, mmd.totlvl, layer_index++, layer.name,
          int((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0),
          double(layer.strength), layer.grids_num, samples, energy, signature);
    }
  }
  fflush(stdout);
}

static void step_decode_undo_impl(bContext *C, Depsgraph *depsgraph, SculptUndoStep *us)
{
  BLI_assert(us->step.is_applied == true);

  sculpt_layer_undo_trace_state(C, *us, "UNDO_BEFORE");
  restore_list(C, depsgraph, us->data);
  sculpt_layer_undo_trace_state(C, *us, "UNDO_AFTER");
  us->step.is_applied = false;
}

static void step_decode_redo_impl(bContext *C, Depsgraph *depsgraph, SculptUndoStep *us)
{
  BLI_assert(us->step.is_applied == false);

  sculpt_layer_undo_trace_state(C, *us, "REDO_BEFORE");
  restore_list(C, depsgraph, us->data);
  sculpt_layer_undo_trace_state(C, *us, "REDO_AFTER");
  us->step.is_applied = true;
}

/* SCULPT_LAYER_UNDO_BOUNDARY_V31
 *
 * The first Position undo after entering Sculpt Mode lands on the Type::None
 * Sculpt boundary. That boundary can still represent the modifier state that
 * existed when Sculpt Mode was entered (for example Strength=1 and max
 * sculptlvl). Keep Position undo geometric: when crossing that boundary,
 * re-apply the frame captured at stroke begin instead of letting the boundary
 * change Sculpt Layer weights or display level.
 */
static void sculpt_layer_restore_position_boundary_frame(bContext *C,
                                                         Depsgraph *depsgraph,
                                                         const StepData &step_data)
{
  if (step_data.type != Type::Position || step_data.layer_frame.is_empty() ||
      step_data.native_multires_base_undo) {
    return;
  }

  Object *object = CTX_data_active_object(C);
  if (!object) {
    return;
  }

  MultiresModifierData *mmd = nullptr;
  for (ModifierData &md : object->modifiers) {
    if (md.type == eModifierType_Multires && md.name == step_data.layer_frame_modifier) {
      mmd = reinterpret_cast<MultiresModifierData *>(&md);
      break;
    }
  }
  if (!mmd || mmd->sculpt_layers.count() != step_data.layer_frame.size()) {
    std::printf("[LAYER_UNDO_BOUNDARY] mode=SKIP reason=STACK_MISMATCH\n");
    return;
  }

  int i = 0;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    if (layer.name != step_data.layer_frame[i++].name) {
      std::printf("[LAYER_UNDO_BOUNDARY] mode=SKIP reason=LAYER_MISMATCH\n");
      return;
    }
  }

  const int wanted_level = std::clamp(step_data.layer_frame_level, 0, int(mmd->totlvl));
  const bool level_changed = mmd->sculptlvl != wanted_level;
  const bool active_changed = mmd->sculpt_layer_active != step_data.layer_frame_active;
  bool weights_changed = false;
  bool changed = level_changed || active_changed;

  i = 0;
  for (const MultiresSculptLayer &layer : mmd->sculpt_layers) {
    const SculptLayerUndoFrameItem &saved = step_data.layer_frame[i++];
    const bool layer_changed = layer.strength != saved.strength ||
                              (((layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0) != saved.visible);
    weights_changed |= layer_changed;
    changed |= layer_changed;
  }

  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  if (trace_env && trace_env[0] && trace_env[0] != '0') {
    std::printf("[LAYER_UNDO_BOUNDARY_CHECK] level_changed=%d weights_changed=%d active_changed=%d\n",
                int(level_changed), int(weights_changed), int(active_changed));
    std::fflush(stdout);
  }

  if (!changed) {
    std::printf("[LAYER_UNDO_BOUNDARY] mode=PRESERVE no_change=1 level=%d\n", wanted_level);
    return;
  }

  /* Only an active-index correction is surface-neutral. Flush the old frame before changing metadata;
   * avoid freeing the CCG/PBVH after a certified local
   * publication or an already clean CCG. Changed levels/weights still rebuild. */
  const char *boundary_env = std::getenv("BLENDER_SCULPT_UNDO_BOUNDARY_METADATA");
  const bool boundary_fast = boundary_env && boundary_env[0] == '1' && boundary_env[1] == '\0';
  SculptSession *boundary_ss = object->runtime->sculpt_session;
  SubdivCCG *boundary_ccg = boundary_ss ? boundary_ss->subdiv_ccg : nullptr;
  if (boundary_fast && !level_changed && !weights_changed && boundary_ss && boundary_ccg &&
      !boundary_ss->cache && !boundary_ss->filter_cache && !boundary_ss->expand_cache &&
      mmd->totlvl == step_data.layer_frame_top_level &&
      boundary_ccg->level == wanted_level &&
      boundary_ccg->grids_num == step_data.grids.grids_num &&
      boundary_ccg->grid_size == step_data.grids.grid_size &&
      !boundary_ccg->dirty.hidden && !boundary_ccg->dirty.masks)
  {
    const bool was_clean = !boundary_ccg->dirty.coords;
    bool local_surface_preserved = false;
    if (multires_flush_sculpt_updates(object, &local_surface_preserved) &&
        (was_clean || local_surface_preserved))
    {
      mmd->sculpt_layer_active = step_data.layer_frame_active;
      bke::multires_layers::weight_hot_delta_cache_invalidate();
      DEG_id_tag_update(&object->id, ID_RECALC_SYNC_TO_EVAL);
      WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, object);
      std::printf("[LAYER_UNDO_BOUNDARY] mode=METADATA_ONLY active=%d\n", mmd->sculpt_layer_active);
      std::fflush(stdout);
      return;
    }
  }

  /* Native CCG undo can leave restored coordinates pending. Flush/release the CCG
   * while its level and weights still match the current modifier. Changing those
   * first can make the flush reject the level or solve deltas against wrong weights. */
  if (!multires_force_sculpt_rebuild(object)) {
    std::printf("[LAYER_UNDO_BOUNDARY] mode=SKIP reason=OLD_FRAME_FLUSH_FAILED pending_retained=1\n");
    return;
  }

  mmd->sculptlvl = wanted_level;
  mmd->sculpt_layer_active = step_data.layer_frame_active;

  i = 0;
  for (MultiresSculptLayer &layer : mmd->sculpt_layers) {
    const SculptLayerUndoFrameItem &saved = step_data.layer_frame[i++];
    layer.strength = saved.strength;
    if (saved.visible) {
      layer.flag |= MULTIRES_SCULPT_LAYER_VISIBLE;
    }
    else {
      layer.flag &= ~MULTIRES_SCULPT_LAYER_VISIBLE;
    }
  }

  bke::multires_layers::weight_hot_delta_cache_invalidate();

  /* The old-frame CCG was already flushed/released before metadata changed.
   * Evaluate the captured frame from that now-coherent persistent state. */
  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  BKE_scene_graph_evaluated_ensure(depsgraph, CTX_data_main(C));
  WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, object);

  const float active_strength =
      (step_data.layer_frame_active >= 0 &&
       step_data.layer_frame_active < step_data.layer_frame.size()) ?
          step_data.layer_frame[step_data.layer_frame_active].strength :
          0.0f;
  std::printf(
      "[LAYER_UNDO_BOUNDARY] mode=RESTORE_FIRST_POSITION level=%d active=%d strength=%.6f\n",
      wanted_level,
      step_data.layer_frame_active,
      active_strength);
}
static void step_decode_undo(bContext *C,
                             Depsgraph *depsgraph,
                             SculptUndoStep *us,
                             const bool is_final)
{
  /* Walk forward over any applied steps of same type,
   * then walk back in the next loop, un-applying them. */
  SculptUndoStep *us_iter = us;
  while (us_iter->step.next && (us_iter->step.next->type == us_iter->step.type)) {
    if (us_iter->step.next->is_applied == false) {
      break;
    }
    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.next);
  }

  while ((us_iter != us) || (!is_final && us_iter == us)) {
    BLI_assert(us_iter->step.type == us->step.type); /* Previous loop ensures this. */

    set_active_layer(C, &us_iter->active_color_start);
    step_decode_undo_impl(C, depsgraph, us_iter);

    if (us_iter == us) {
      if (us_iter->step.prev && us_iter->step.prev->type == BKE_UNDOSYS_TYPE_SCULPT) {
        SculptUndoStep *prev = reinterpret_cast<SculptUndoStep *>(us_iter->step.prev);
        if (us_iter->data.type == Type::Position && prev->data.type == Type::None) {
          sculpt_layer_undo_trace_state(C, *us_iter, "BOUNDARY_BEFORE");
          sculpt_layer_restore_position_boundary_frame(C, depsgraph, us_iter->data);
          sculpt_layer_undo_trace_state(C, *us_iter, "BOUNDARY_AFTER");
        }
        set_active_layer(C, &prev->active_color_end);
      }
      break;
    }

    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.prev);
  }
}

static void step_decode_redo(bContext *C, Depsgraph *depsgraph, SculptUndoStep *us)
{
  SculptUndoStep *us_iter = us;
  while (us_iter->step.prev && (us_iter->step.prev->type == us_iter->step.type)) {
    if (us_iter->step.prev->is_applied == true) {
      break;
    }
    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.prev);
  }
  while (us_iter && (us_iter->step.is_applied == false)) {
    set_active_layer(C, &us_iter->active_color_end);
    step_decode_redo_impl(C, depsgraph, us_iter);

    if (us_iter == us) {
      if (us_iter->step.prev && us_iter->step.prev->type == BKE_UNDOSYS_TYPE_SCULPT) {
        SculptUndoStep *prev = reinterpret_cast<SculptUndoStep *>(us_iter->step.prev);
        if (us_iter->data.type == Type::Position && prev->data.type == Type::None) {
          sculpt_layer_restore_position_boundary_frame(C, depsgraph, us_iter->data);
        }
      }
      set_active_layer(C, &us_iter->active_color_start);
      break;
    }
    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.next);
  }
}

static void step_decode(
    bContext *C, Main *bmain, UndoStep *us_p, const eUndoStepDir dir, const bool is_final)
{
  const char *trace_env = std::getenv("BLENDER_LAYER_RESP_TRACE");
  const bool trace = trace_env && trace_env[0] && trace_env[0] != '0';
  const double trace_start = trace ? BLI_time_now_seconds() : 0.0;
  BLI_SCOPED_DEFER([&]() {
    if (trace) {
      std::printf("[LAYER_SCULPT_DECODE] direction=%d step=%s total=%.3fms\n",
                  int(dir), us_p->name, (BLI_time_now_seconds() - trace_start) * 1000.0);
      std::fflush(stdout);
    }
  });
  /* NOTE: behavior for undo/redo closely matches image undo. */
  BLI_assert(dir != STEP_INVALID);
  sculpt_layer_undo_trace_state(
      C, *reinterpret_cast<const SculptUndoStep *>(us_p), "DECODE_ENTER");

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  /* Ensure sculpt mode. */
  {
    Scene *scene = CTX_data_scene(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    Object *ob = BKE_view_layer_active_object_get(view_layer);
    if (ob && (ob->type == OB_MESH)) {
      if (ob->mode & (OB_MODE_SCULPT)) {
        /* Pass. */
      }
      else {
        object::mode_generic_exit(bmain, depsgraph, scene, ob);

        /* Sculpt needs evaluated state.
         * NOTE: needs to be done here, as #object::mode_generic_exit will usually invalidate
         * (some) evaluated data. */
        BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

        Mesh *mesh = id_cast<Mesh *>(ob->data);
        /* Don't add sculpt topology undo steps when reading back undo state.
         * The undo steps must enter/exit for us. */
        mesh->flag &= ~ME_SCULPT_DYNAMIC_TOPOLOGY;
        object_sculpt_mode_enter(*bmain, *depsgraph, *scene, *ob, true, nullptr);
      }

      if (ob->runtime->sculpt_session) {
        ob->runtime->sculpt_session->needs_flush_to_id = true;
      }
      bmain->is_memfile_undo_flush_needed = true;
    }
    else {
      BLI_assert(0);
      return;
    }
  }

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);
  sculpt_layer_undo_trace_state(C, *us, "DECODE_READY");
  if (trace && us->data.layer_add_boundary_anchor) {
    std::printf("[LAYER_UNDO_ANCHOR] mode=DECODE direction=%d final=%d step=%s\n",
                int(dir), int(is_final), us_p->name);
    std::fflush(stdout);
  }
  if (dir == STEP_UNDO) {
    step_decode_undo(C, depsgraph, us, is_final);
  }
  else if (dir == STEP_REDO) {
    step_decode_redo(C, depsgraph, us);
  }
}

static void step_free(UndoStep *us_p)
{
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);
  free_step_data(us->data);
}

void geometry_begin(const Scene &scene, Object &ob, const wmOperator *op)
{
  geometry_begin_ex(scene, ob, op->type->name);
}

void geometry_begin_ex(const Scene & /*scene*/, Object &ob, const char *name)
{
  UndoStack *ustack = ED_undo_stack_get();

  /* If possible, we need to tag the object and its geometry data as 'changed in the future' in
   * the previous undo step if it's a memfile one. */
  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  /* Special case, we never read from this. */
  bContext *C = nullptr;

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_SCULPT));
  save_common_data(ob, us);
  geometry_push(ob);
}

static size_t calculate_node_geometry_allocated_size(const NodeGeometry &node_geometry)
{
  BLI_assert(node_geometry.is_initialized);

  MemoryCount memory;
  MemoryCounter memory_counter(memory);

  memory_counter.add_shared(node_geometry.face_offsets_sharing_info,
                            sizeof(int) * (node_geometry.faces_num + 1));

  CustomData_count_memory(node_geometry.corner_data, node_geometry.corners_num, memory_counter);
  CustomData_count_memory(node_geometry.face_data, node_geometry.faces_num, memory_counter);
  CustomData_count_memory(node_geometry.vert_data, node_geometry.verts_num, memory_counter);
  CustomData_count_memory(node_geometry.edge_data, node_geometry.edges_num, memory_counter);
  node_geometry.attribute_storage.wrap().count_memory(memory_counter);

  return memory.total_bytes;
}

static size_t estimate_geometry_step_size(const StepData &step_data)
{
  size_t step_size = 0;

  /* TODO: This calculation is not entirely accurate, as the current amount of memory consumed by
   * Sculpt Undo is not updated when elements are evicted. Further changes to the overall undo
   * system would be needed to measure this accurately. */
  step_size += calculate_node_geometry_allocated_size(step_data.geometry_original);
  step_size += calculate_node_geometry_allocated_size(step_data.geometry_modified);

  return step_size;
}

void geometry_end(Object &ob)
{
  geometry_push(ob);

  StepData *step_data = get_step_data();
  step_data->undo_size = estimate_geometry_step_size(*step_data);

  /* We could remove this and enforce all callers run in an operator using 'OPTYPE_UNDO'. */
  wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
  if (wm->op_undo_depth == 0) {
    UndoStack *ustack = ED_undo_stack_get();
    BKE_undosys_step_push(ustack, nullptr, nullptr);
    if (wm->op_undo_depth == 0) {
      BKE_undosys_stack_limit_steps_and_memory_defaults(ustack);
    }
    WM_file_tag_modified();
  }
}

void register_type(UndoType *ut)
{
  ut->name = "Sculpt";
  ut->poll = nullptr; /* No poll from context for now. */
  ut->step_encode_init = step_encode_init;
  ut->step_encode = step_encode;
  ut->step_decode = step_decode;
  ut->step_free = step_free;

  ut->flags = UNDOTYPE_FLAG_DECODE_ACTIVE_STEP;

  ut->step_size = sizeof(SculptUndoStep);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multires + Base Mesh Undo
 *
 * Example of a relevant operator is Apply Base.
 *
 * Usage:
 *
 *   static int operator_exec(bContext *C, wmOperator *op) {
 *
 *      ed::sculpt_paint::undo::push_multires_mesh_begin(C, op->type->name);
 *      // Modify base mesh.
 *      ed::sculpt_paint::undo::push_multires_mesh_end(C, op->type->name);
 *
 *      return OPERATOR_FINISHED;
 *   }
 *
 * If object is not in Sculpt mode or there is no active multires object, ED_undo_push is used
 * instead.
 * \{ */

static bool use_multires_mesh(bContext *C)
{
  if (BKE_paintmode_get_active_from_context(C) != PaintMode::Sculpt) {
    return false;
  }

  const Object *object = CTX_data_active_object(C);
  const SculptSession *sculpt_session = object->runtime->sculpt_session;

  return sculpt_session->multires_modifier;
}

void push_multires_mesh_begin(bContext *C, const char *str)
{
  if (!use_multires_mesh(C)) {
    return;
  }

  const Scene &scene = *CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);

  multires_flush_sculpt_updates(object);

  push_begin_ex(scene, *object, str);

  geometry_push(*object);
}

void push_multires_mesh_end(bContext *C, const char *str)
{
  if (!use_multires_mesh(C)) {
    ED_undo_push(C, str);
    return;
  }

  Object *object = CTX_data_active_object(C);

  geometry_push(*object);

  push_end(*object);
}

/** \} */

}  // namespace ed::sculpt_paint::undo

namespace ed::sculpt_paint {

std::optional<OrigPositionData> orig_position_data_lookup_mesh_all_verts(
    const Object & /*object*/, const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::Position);
  if (!unode) {
    return std::nullopt;
  }
  return OrigPositionData{unode->position.as_span(), unode->normal.as_span()};
}

std::optional<OrigPositionData> orig_position_data_lookup_mesh(const Object &object,
                                                               const bke::pbvh::MeshNode &node)
{
  const std::optional<OrigPositionData> result = orig_position_data_lookup_mesh_all_verts(object,
                                                                                          node);
  if (!result) {
    return std::nullopt;
  }
  return OrigPositionData{result->positions.take_front(node.verts().size()),
                          result->normals.take_front(node.verts().size())};
}

std::optional<OrigPositionData> orig_position_data_lookup_grids(const Object & /*object*/,
                                                                const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::Position);
  if (!unode) {
    return std::nullopt;
  }
  return OrigPositionData{unode->position.as_span(), unode->normal.as_span()};
}

void orig_position_data_gather_bmesh(const BMLog &bm_log,
                                     const Set<BMVert *, 0> &verts,
                                     const MutableSpan<float3> positions,
                                     const MutableSpan<float3> normals)
{
  int i = 0;
  for (const BMVert *vert : verts) {
    const float *co;
    const float *no;
    BM_log_original_vert_data(&const_cast<BMLog &>(bm_log), const_cast<BMVert *>(vert), &co, &no);
    if (!positions.is_empty()) {
      positions[i] = co;
    }
    if (!normals.is_empty()) {
      normals[i] = no;
    }
    i++;
  }
}

std::optional<Span<float4>> orig_color_data_lookup_mesh(const Object & /*object*/,
                                                        const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::Color);
  if (!unode) {
    return std::nullopt;
  }
  return unode->col.as_span();
}

std::optional<Span<float4>> orig_color_data_lookup_grids(const Object &object,
                                                         const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::Color);
  const SculptSession *ss = object.runtime->sculpt_session;
  if (!unode || !ss || !ss->subdiv_ccg ||
      unode->grid_color_side != ss->subdiv_ccg->grid_size)
  {
    return std::nullopt;
  }
  /* Missing grids are represented by the base attribute, not by the zeroed
   * slots in Undo's master snapshot. Callers must not treat those as paint. */
  for (const int i : unode->grids.index_range()) {
    if (!unode->grid_color_present[i]) {
      return std::nullopt;
    }
  }
  return unode->col.as_span();
}

std::optional<Span<int>> orig_face_set_data_lookup_mesh(const Object & /*object*/,
                                                        const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::FaceSet);
  if (!unode) {
    return std::nullopt;
  }
  return unode->face_sets.as_span();
}

std::optional<Span<int>> orig_face_set_data_lookup_grids(const Object & /*object*/,
                                                         const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::FaceSet);
  if (!unode) {
    return std::nullopt;
  }
  return unode->face_sets.as_span();
}

std::optional<Span<float>> orig_mask_data_lookup_mesh(const Object & /*object*/,
                                                      const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::Mask);
  if (!unode) {
    return std::nullopt;
  }
  return unode->mask.as_span();
}

std::optional<Span<float>> orig_mask_data_lookup_grids(const Object & /*object*/,
                                                       const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(&node, undo::Type::Mask);
  if (!unode) {
    return std::nullopt;
  }
  return unode->mask.as_span();
}

}  // namespace ed::sculpt_paint

}  // namespace blender





