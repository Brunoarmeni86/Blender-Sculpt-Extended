/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BLI_index_mask_fwd.hh"

struct Brush;
struct Depsgraph;
struct Object;
struct Sculpt;

namespace blender::ed::sculpt_paint::brushes {

/* Optional post-deformation material response shared by the Clay family.
 * Standard brush deformation is already committed when this is called. */
bool apply_physical_material_finish(const Depsgraph &depsgraph,
                                    const Sculpt &sd,
                                    const Brush &brush,
                                    Object &object,
                                    const IndexMask &node_mask);

/* Surface-only experimental cut opening. It never changes topology; the
 * standard Crease/Draw Sharp result remains the source deformation. */
bool apply_physical_lesion_preview(const Depsgraph &depsgraph,
                                   const Sculpt &sd,
                                   const Brush &brush,
                                   Object &object,
                                   const IndexMask &node_mask);

}  // namespace blender::ed::sculpt_paint::brushes
