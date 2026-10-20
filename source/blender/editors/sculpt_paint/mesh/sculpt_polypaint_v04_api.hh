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
MultiresModifierData *color_v04_modifier(Object &ob);
bool color_v04_export(bContext *C,wmOperator *op);
bool remove_attribute_full_v04(bContext *C,wmOperator *op,const char *name);
}
namespace undo {bool capture_color_master_v04(Object &object,const int *grids,int count);}
}
