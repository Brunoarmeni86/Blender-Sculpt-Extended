/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>

namespace blender::bke::multires_layers::region_v01 {

/* Visit exactly the same vertex-adjacent coarse-face ring as the legacy full
 * face scan. The caller supplies freshly zeroed bits, validated grid indices,
 * and the original mesh's topology. No persistent topology cache is retained.
 * Callbacks run once per distinct face; their order is deliberately unspecified.
 * Keep this independent of Blender containers so the production algorithm can
 * also be checked against an exhaustive reference on synthetic topologies. */
template<typename Faces,
         typename CornerVerts,
         typename VertToFace,
         typename Grids,
         typename Bits,
         typename SupportCallback,
         typename AffectedCallback>
void visit_ring(const Faces &faces,
                const CornerVerts &corner_verts,
                const VertToFace &vert_to_face,
                const Grids &payload_grids,
                Bits &support,
                Bits &affected,
                SupportCallback emit_support,
                AffectedCallback emit_affected)
{
  const auto &offsets = faces.data();
  const auto add_affected = [&](const int face) {
    if (!affected[face]) {
      affected[face].set();
      emit_affected(face);
    }
  };
  for (const int grid : payload_grids) {
    const int face = int(std::upper_bound(offsets.begin(), offsets.end(), grid) -
                         offsets.begin() - 1);
    if (support[face]) {
      continue;
    }
    support[face].set();
    emit_support(face);
    add_affected(face);
    for (const int corner : faces[face]) {
      for (const int adjacent : vert_to_face[corner_verts[corner]]) {
        add_affected(adjacent);
      }
    }
  }
}

}  // namespace blender::bke::multires_layers::region_v01
