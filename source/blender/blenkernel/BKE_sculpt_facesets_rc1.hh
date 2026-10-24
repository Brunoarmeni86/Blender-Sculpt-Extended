/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "BKE_mesh.hh"
#include "BKE_multires_sculpt_layers.hh"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
namespace blender::bke::facesets_rc1 {
inline bool enabled()
{
  const char *flag=std::getenv("BLENDER_FACESETS_RC1");
  return flag && std::strcmp(flag,"1")==0;
}
inline MultiresModifierData *modifier(Object &object)
{
  if (!object.modifiers.is_single()) { return nullptr; }
  ModifierData &md=*static_cast<ModifierData *>(object.modifiers.first);
  if (md.type==eModifierType_Multires && (md.mode&eModifierMode_Realtime) &&
      !(md.mode&eModifierMode_DisableTemporary)) {
    return reinterpret_cast<MultiresModifierData *>(&md);
  }
  return nullptr;
}
inline bool eligible(const Object &object,const Mesh &mesh,const MultiresModifierData &mmd)
{
  if (!enabled() || object.type!=OB_MESH || !object.modifiers.is_single() ||
      object.id.lib || object.id.override_library || mesh.id.lib || mesh.id.override_library ||
      mesh.key || mesh.corners_num<=0 || mmd.totlvl<1 || mmd.totlvl>8 ||
      (mmd.flags&eMultiresModifierFlag_UseSculptBaseMesh) ||
      mmd.sculpt_displacement_enabled || mmd.sculpt_shrinkwrap_enabled) { return false; }
  const int64_t side=(int64_t(1)<<(mmd.totlvl-1))+1;
  return int64_t(mesh.corners_num)*side*side<=16777216;
}
inline bool master_valid(const Object &object,const Mesh &mesh,const MultiresModifierData &mmd)
{
  /* A retained higher-resolution categorical master remains valid after
   * Delete Higher. Lower sculpt grids sample it exactly and preserve IDs. */
  return eligible(object,mesh,mmd) && mmd.sculpt_face_set_level>=mmd.totlvl &&
         multires_layers::faceset_master_valid(mmd,mesh.corners_num);
}
}  // namespace blender::bke::facesets_rc1
