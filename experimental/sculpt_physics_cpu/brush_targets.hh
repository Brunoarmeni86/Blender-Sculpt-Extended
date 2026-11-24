/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "physics_core.hh"

#include <cstddef>
#include <vector>

namespace sculpt_physics {

enum class AssistedBrushKind { Grab, Clay, ClayStrips, Smooth, GranularDrag };

/* Blender supplies brush-selected vertices and final mask/falloff factors.
 * A factor of zero is a true no-op; hidden and pinned vertices never move. */
struct BrushVertexInput {
  std::size_t particle_index = 0;
  Vec3 normal{};
  double factor = 0.0;
};

struct AssistedDab {
  AssistedBrushKind kind = AssistedBrushKind::Grab;
  Vec3 center{};
  Vec3 normal{0.0, 0.0, 1.0};
  Vec3 stroke_direction{1.0, 0.0, 0.0};
  Vec3 drag_delta{};
  double radius = 1.0;
  double strength = 1.0;
  double compliance = 1.0e-6;
};

/* One-time edge adjacency construction per topology generation/stroke.
 * Broken bonds are ignored for Smooth; queries don't scan the entire mesh. */
class BrushAdjacency {
 public:
  explicit BrushAdjacency(const World &world);
  std::size_t size() const { return neighbors_.size(); }
  const std::vector<std::size_t> &neighbors(std::size_t index) const;

 private:
  std::vector<std::vector<std::size_t>> neighbors_;
};

/* Builds pure target goals; never mutates mesh, solver, Undo or Sculpt Layers.
 * Standard and 0%-Assist bypass without a solver call. In PhysicalAssist,
 * influence is applied ONCE by SculptSurfaceBridge after physics; in
 * FullPhysics it scales the brush goal here. Caller must call World::begin_stroke,
 * set_brush_targets, step_local and compose/writeback transactionally. */
std::vector<World::BrushTargetInput> plan_assisted_brush(
    const World &world,
    const BrushAdjacency &topology,
    const std::vector<BrushVertexInput> &selection,
    const AssistedDab &dab,
    BrushPhysicsSettings settings);

}  // namespace sculpt_physics
