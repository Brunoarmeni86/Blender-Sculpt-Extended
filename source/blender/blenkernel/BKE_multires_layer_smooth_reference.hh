/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "BLI_math_vector_types.hh"
namespace blender {
struct Mesh;
struct MultiresSculptLayer;
struct SubdivCCG;
namespace bke::multires_layers {
struct SmoothL0Reference;
/** One coarse reference per live CCG, not one dense surface per layer.
 * Exact input comparison prevents stale data after weights, Undo or L0 edits.
 * The live CCG owns the topology; clear MUST run before its Subdiv is freed. */
bool smooth_l0_reference_prepare(const MultiresSculptLayer &layer,
                                 const Mesh &mesh,
                                 SubdivCCG &ccg,
                                 const SmoothL0Reference *&r_reference);
float3 smooth_l0_reference_sample(const SmoothL0Reference &reference, int ptex, float u, float v);
void smooth_l0_reference_clear(const SubdivCCG &ccg);
}
}
