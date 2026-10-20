/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdlib>
#include <climits>
#include <cstring>
struct wmOperatorType;
namespace blender::ed::sculpt_paint::color {
inline bool polypaint_blur_v01_enabled()
{
  const char *v=std::getenv("BLENDER_SCULPT_POLYPAINT_BLUR_V01");
  return v && std::strcmp(v,"1")==0;
}
void SCULPT_OT_polypaint_blur_probe_v01(wmOperatorType *ot);
}

#include "sculpt_polypaint_v04_api.hh"
