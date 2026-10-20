/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdlib>
#include <cstdint>
#include <cstring>

#include "BKE_mesh.hh"
#include "BKE_sculpt_facesets_rc1.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "BKE_object_types.hh"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

/* Bounded diagnostic fixture, not a production-mesh enable switch. */
namespace blender::bke::facesets_dense_v06d {
inline bool enabled()
{
  const char *flag = std::getenv("BLENDER_FACESETS_DENSE_V06D");
  return flag && std::strcmp(flag, "1") == 0;
}
/* V06e: explicit bounded mesh trial; does not enable this path at normal startup. */
inline bool mesh_trial_enabled()
{
  const char *flag = std::getenv("BLENDER_FACESETS_MESH_TRIAL_V06E");
  return flag && std::strcmp(flag,"1") == 0;
}
inline bool mesh_trial_shape_valid(const Object &object,const Mesh &mesh,
                                   const MultiresModifierData &mmd)
{
  if (!mesh_trial_enabled() || object.type!=OB_MESH || object.id.lib || mesh.id.lib ||
      mesh.key || !mmd.sculpt_layers.is_empty() || mmd.totlvl<1 || mmd.totlvl>5 ||
      mesh.verts_num<=0 || mesh.faces_num<=0 || mesh.corners_num<=0 ||
      mesh.corners_num>4096) { return false; }
  const int64_t side=(int64_t(1)<<(mmd.totlvl-1))+1;
  return int64_t(mesh.corners_num)*side*side<=262144;
}
inline bool shape_valid(const Object &object,
                        const Mesh &mesh,
                        const MultiresModifierData &mmd)
{
  if (facesets_rc1::eligible(object,mesh,mmd)) { return true; }
  if (mesh_trial_shape_valid(object,mesh,mmd)) { return true; }
  return enabled() && object.type == OB_MESH &&
         std::strcmp(object.id.name + 2, "FaceSets_Rebuild_Dense_Test") == 0 &&
         !object.id.lib && !mesh.id.lib && !mesh.key && mesh.verts_num == 98 &&
         mesh.faces_num == 96 && mesh.corners_num == 384 && mmd.totlvl == 3 &&
         mmd.sculpt_layers.is_empty();
}
inline bool master_valid(const Object &object,
                         const Mesh &mesh,
                         const MultiresModifierData &mmd)
{
  return shape_valid(object, mesh, mmd) && mmd.sculpt_face_set_level == mmd.totlvl &&
         multires_layers::faceset_master_valid(mmd, mesh.corners_num);
}
}  // namespace blender::bke::facesets_dense_v06d
