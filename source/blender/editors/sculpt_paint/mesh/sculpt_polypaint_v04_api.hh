/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdlib>
#include <cstring>
namespace blender {
struct MultiresModifierData;struct Object;struct bContext;struct wmOperator;
}
namespace blender::ed::sculpt_paint {
namespace color {
inline bool polypaint_v04_enabled(){const char *v=std::getenv("BLENDER_SCULPT_POLYPAINT_V04");return v&&std::strcmp(v,"1")==0;}
inline bool polypaint_v04_diagnostics_enabled(){
 static const bool enabled=[](){const char *v=std::getenv("BLENDER_SCULPT_POLYPAINT_V04_DIAGNOSTICS");return v&&std::strcmp(v,"1")==0;}();
 return enabled;
}
MultiresModifierData *color_v04_modifier(Object &ob);
bool color_v04_export(bContext *C,wmOperator *op);
bool remove_attribute_full_v04(bContext *C,wmOperator *op,const char *name);
}
namespace undo {
bool capture_color_master_region_v04(
    Object &object, int grid, int x, int y, int width, int height);
}
}
