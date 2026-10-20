/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstring>
#include "BLI_path_utils.hh"

namespace blender::sculpt_extended_release {
inline void initialize_defaults()
{
  /* Keep explicit developer/user overrides. Run before any feature cache or worker is initialized. */
  const char *native = BLI_getenv("BLENDER_SCULPT_EXTENDED_NATIVE");
  if (native && std::strcmp(native, "0") == 0) {
    return;
  }
  BLI_setenv_if_new("BLENDER_SCULPT_DRAW_TRANSPORT_V01", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_V04", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_L0_TRACE", "0");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_L0_DRAW_V04D", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_BLUR_V01", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_CPU", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_LAZY_MASTER", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_MULTI_ATTRIBUTE", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_PARALLEL_V3", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_UNDO_COPY_V3", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3", "1");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_PREPARED_V2", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_TOOLS_RC10", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_RC1", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_VISIBILITY_RC2", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_NATIVE_HIDE_RC4", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_GESTURES_RC5", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_BRUSH_V05", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_DISPLAY_V05", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_QUAD_DISPLAY_V06C", "1");
  BLI_setenv_if_new("BLENDER_FACESETS_DENSE_V06D", "0");
  BLI_setenv_if_new("BLENDER_FACESETS_MESH_TRIAL_V06E", "0");
  BLI_setenv_if_new("BLENDER_MULTIRES_FACESETS_DRAW_V01", "0");
  BLI_setenv_if_new("BLENDER_MULTIRES_FACESETS_VISIBILITY_V02", "0");
  BLI_setenv_if_new("BLENDER_MULTIRES_FACESETS_GESTURES_V03", "0");
  BLI_setenv_if_new("FACESETS_RC1_AUTOCLOSE", "0");
  BLI_setenv_if_new("BLENDER_SCULPT_POLYPAINT_WORK_MB", "512");
  BLI_setenv_if_new("FACESETS_RC1_PREVIEW", "0");
  BLI_setenv_if_new("BLENDER_SCULPT_EXTENDED_NATIVE", "1");
}
}  // namespace blender::sculpt_extended_release
