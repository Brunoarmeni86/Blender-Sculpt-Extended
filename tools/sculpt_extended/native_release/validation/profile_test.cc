#include <cassert>
#include <iostream>
#include "../payload/sculpt_extended_profile.hh"
int main(){using blender::sculpt_extended_release::initialize_defaults;initialize_defaults();
assert(env["BLENDER_SCULPT_POLYPAINT_V04"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_L0_TRACE"]=="0");
assert(env["BLENDER_SCULPT_POLYPAINT_L0_DRAW_V04D"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_BLUR_V01"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_CPU"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_LAZY_MASTER"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_MULTI_ATTRIBUTE"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_PARALLEL_V3"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_UNDO_COPY_V3"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_LOCAL_REDRAW_V3"]=="1");
assert(env["BLENDER_SCULPT_POLYPAINT_PREPARED_V2"]=="1");
assert(env["BLENDER_FACESETS_TOOLS_RC10"]=="1");
assert(env["BLENDER_FACESETS_RC1"]=="1");
assert(env["BLENDER_FACESETS_VISIBILITY_RC2"]=="1");
assert(env["BLENDER_FACESETS_NATIVE_HIDE_RC4"]=="1");
assert(env["BLENDER_FACESETS_GESTURES_RC5"]=="1");
assert(env["BLENDER_FACESETS_BRUSH_V05"]=="1");
assert(env["BLENDER_FACESETS_DISPLAY_V05"]=="1");
assert(env["BLENDER_FACESETS_QUAD_DISPLAY_V06C"]=="1");
assert(env["BLENDER_FACESETS_DENSE_V06D"]=="0");
assert(env["BLENDER_FACESETS_MESH_TRIAL_V06E"]=="0");
assert(env["BLENDER_MULTIRES_FACESETS_DRAW_V01"]=="0");
assert(env["BLENDER_MULTIRES_FACESETS_VISIBILITY_V02"]=="0");
assert(env["BLENDER_MULTIRES_FACESETS_GESTURES_V03"]=="0");
assert(env["FACESETS_RC1_AUTOCLOSE"]=="0");
assert(env["BLENDER_SCULPT_POLYPAINT_WORK_MB"]=="512");
assert(env["FACESETS_RC1_PREVIEW"]=="0");
assert(env["BLENDER_SCULPT_EXTENDED_NATIVE"]=="1");env.clear();env["BLENDER_SCULPT_POLYPAINT_WORK_MB"]="256";env["BLENDER_SCULPT_POLYPAINT_V04"]="0";initialize_defaults();assert(env["BLENDER_SCULPT_POLYPAINT_WORK_MB"]=="256");assert(env["BLENDER_SCULPT_POLYPAINT_V04"]=="0");env.clear();env["BLENDER_SCULPT_EXTENDED_NATIVE"]="0";initialize_defaults();assert(env.size()==1);std::cout<<"PASS 27 defaults, explicit overrides, opt-out\n";}
