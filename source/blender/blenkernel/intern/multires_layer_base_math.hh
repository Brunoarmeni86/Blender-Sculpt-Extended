/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <cmath>
#include <vector>

/* L0 deltas are in cage/object space, never tangent-space CCG samples.
 * This small kernel is shared by Blender integration and standalone validation.
 * Writes are transactional: rejected input never publishes a partial edit. */
namespace blender::bke::multires_layers::base_math {

using Position = std::array<float, 3>;
struct LayerView {
  const float (*deltas)[3];
  int count;
  float strength;
  bool visible;
};

inline bool finite(const float *value)
{
  return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

inline bool compose(const float (*base)[3],
                    const int count,
                    const std::vector<LayerView> &layers,
                    float (*result)[3])
{
  if (count < 0 || (count && (!base || !result))) {
    return false;
  }
  for (const LayerView &layer : layers) {
    if (layer.count != count || (count && !layer.deltas) ||
        !std::isfinite(layer.strength))
    {
      return false;
    }
  }
  std::vector<Position> staged(count);
  for (int vertex = 0; vertex < count; vertex++) {
    if (!finite(base[vertex])) {
      return false;
    }
    for (int axis = 0; axis < 3; axis++) {
      staged[vertex][axis] = base[vertex][axis];
    }
    for (const LayerView &layer : layers) {
      if (!layer.visible) {
        continue;
      }
      if (!finite(layer.deltas[vertex])) {
        return false;
      }
      for (int axis = 0; axis < 3; axis++) {
        staged[vertex][axis] += layer.strength * layer.deltas[vertex][axis];
      }
      if (!finite(staged[vertex].data())) {
        return false;
      }
    }
  }
  for (int vertex = 0; vertex < count; vertex++) {
    for (int axis = 0; axis < 3; axis++) {
      result[vertex][axis] = staged[vertex][axis];
    }
  }
  return true;
}

inline bool record(float (*deltas)[3],
                   const int count,
                   const float strength,
                   const int *vertices,
                   const float (*translations)[3],
                   const int edits)
{
  if (count < 0 || edits < 0 || !std::isfinite(strength) ||
      std::abs(strength) < 1e-6f || (count && !deltas) ||
      (edits && (!vertices || !translations)))
  {
    return false;
  }
  std::vector<bool> seen(count, false);
  std::vector<Position> staged(edits);
  for (int i = 0; i < edits; i++) {
    const int vertex = vertices[i];
    if (vertex < 0 || vertex >= count || seen[vertex] ||
        !finite(deltas[vertex]) || !finite(translations[i]))
    {
      return false;
    }
    seen[vertex] = true;
    for (int axis = 0; axis < 3; axis++) {
      staged[i][axis] = deltas[vertex][axis] + translations[i][axis] / strength;
    }
    if (!finite(staged[i].data())) {
      return false;
    }
  }
  for (int i = 0; i < edits; i++) {
    for (int axis = 0; axis < 3; axis++) {
      deltas[vertices[i]][axis] = staged[i][axis];
    }
  }
  return true;
}

inline bool remap(const float (*source)[3],
                  const int old_count,
                  const int *new_to_old,
                  const int new_count,
                  float (*result)[3])
{
  if (old_count < 0 || new_count < 0 || (old_count && !source) ||
      (new_count && (!new_to_old || !result)))
  {
    return false;
  }
  std::vector<bool> seen(old_count, false);
  std::vector<Position> staged(new_count, Position{0, 0, 0});
  for (int vertex = 0; vertex < new_count; vertex++) {
    const int old_vertex = new_to_old[vertex];
    if (old_vertex == -1) {
      continue;
    }
    if (old_vertex < 0 || old_vertex >= old_count || seen[old_vertex] ||
        !finite(source[old_vertex]))
    {
      return false;
    }
    seen[old_vertex] = true;
    for (int axis = 0; axis < 3; axis++) {
      staged[vertex][axis] = source[old_vertex][axis];
    }
  }
  for (int vertex = 0; vertex < new_count; vertex++) {
    for (int axis = 0; axis < 3; axis++) {
      result[vertex][axis] = staged[vertex][axis];
    }
  }
  return true;
}

}  // namespace blender::bke::multires_layers::base_math
