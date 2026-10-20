/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTable.h>
#include <vector>

/* Conservative dependency propagation. OR nonzero-weight contributions without
 * arithmetic cancellation, then include every affected patch's complete face.
 * This includes dependencies of positions AND derivatives at arbitrary UVs. */
namespace blender::multires_subdivide_support_v03 {
struct Influence {
  unsigned char hit = 0;
  void Clear(void * = nullptr) { hit = 0; }
  void AddWithWeight(const Influence &source, const float weight)
  {
    if (weight != 0.0f && source.hit) {
      hit = 1;
    }
  }
};
inline bool detect(const OpenSubdiv::Far::TopologyRefiner &refiner,
                   const OpenSubdiv::Far::PatchTable &patches,
                   const std::vector<unsigned char> &changed,
                   std::vector<unsigned char> &affected)
{
  const int base_count = refiner.GetLevel(0).GetNumVertices();
  if (int(changed.size()) != base_count) {
    return false;
  }
  const int refined_count = refiner.GetNumVerticesTotal() - base_count;
  if (refined_count < 0) {
    return false;
  }
  std::vector<Influence> coarse(base_count);
  std::vector<Influence> refined(refined_count + patches.GetNumLocalPoints());
  for (int i = 0; i < base_count; ++i) {
    coarse[i].hit = changed[i] != 0;
  }
  OpenSubdiv::Far::PrimvarRefiner interpolation(refiner);
  const Influence *source = coarse.data();
  Influence *target = refined.data();
  for (int level = 1; level < refiner.GetNumLevels(); ++level) {
    interpolation.Interpolate(level, source, target);
    source = target;
    target += refiner.GetLevel(level).GetNumVertices();
  }
  if (patches.GetNumLocalPoints()) {
    patches.GetLocalPointStencilTable()->UpdateValues(coarse.data(), base_count,
                                                      refined.data(),
                                                      refined.data() + refined_count);
  }
  affected.assign(refiner.GetLevel(0).GetNumFaces(), 0);
  for (int array = 0; array < patches.GetNumPatchArrays(); ++array) {
    for (int patch = 0; patch < patches.GetNumPatches(array); ++patch) {
      const auto ids = patches.GetPatchVertices(array, patch);
      for (int i = 0; i < ids.size(); ++i) {
        const int id = ids[i];
        if (id < 0 || id >= base_count + int(refined.size())) {
          return false;
        }
        const bool hit = id < base_count ? coarse[id].hit : refined[id - base_count].hit;
        if (hit) {
          const int face = patches.GetPatchParam(array, patch).GetFaceId();
          if (face < 0 || face >= int(affected.size())) {
            return false; /* This entry point deliberately accepts all-quad reshape topology. */
          }
          affected[face] = 1;
          break;
        }
      }
    }
  }
  return true;
}
}  // namespace blender::multires_subdivide_support_v03
