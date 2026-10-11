/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_multires_sculpt_layers.hh"
#include "DNA_modifier_types.h"
#include "DNA_meshdata_types.h"
#include "MEM_guardedalloc.h"

namespace blender::bke::multires_layers::tests {

class MultiresLayerRemap : public testing::Test {
 protected:
  MultiresModifierData mmd{};
  MultiresSculptLayer *layer = nullptr;

  void SetUp() override
  {
    mmd.totlvl = 1;
    layer = add(mmd, "Detail", 1, 3);
    ASSERT_NE(layer, nullptr);
  }

  void TearDown() override
  {
    clear(mmd);
    polypaint_clear(mmd);
    faceset_clear(mmd);
  }
};

TEST_F(MultiresLayerRemap, SparseSurvivorsAndNewCorners)
{
  const int map[] = {2, -1, 0};
  ASSERT_TRUE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(layer->grids_num, 3);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(layer->disps[i].level, 1);
    EXPECT_EQ(layer->disps[i].totdisp, 4);
    EXPECT_EQ(layer->disps[i].disps, nullptr);
  }
}

TEST_F(MultiresLayerRemap, DensePayloadAndCageDeltasSurvive)
{
  float (*payload)[3] = grid_ensure(*layer, 2);
  ASSERT_NE(payload, nullptr);
  payload[0][0] = 7.5f;
  ASSERT_TRUE(base_deltas_ensure(*layer, 4));
  float (*cage)[3] = layer->base_deltas;
  cage[1][2] = -2.0f;
  const int map[] = {2, -1};
  ASSERT_TRUE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(layer->disps[0].disps, payload);
  EXPECT_FLOAT_EQ(layer->disps[0].disps[0][0], 7.5f);
  EXPECT_EQ(layer->base_deltas, cage);
  EXPECT_EQ(layer->base_verts_num, 4);
  EXPECT_FLOAT_EQ(layer->base_deltas[1][2], -2.0f);
}

TEST_F(MultiresLayerRemap, DuplicateMapIsRejectedBeforeMutation)
{
  const int map[] = {1, 1};
  const MDisps *original = layer->disps;
  EXPECT_FALSE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(layer->disps, original);
  EXPECT_EQ(layer->grids_num, 3);
}

TEST_F(MultiresLayerRemap, InvalidColorStorageDoesNotPublishLayers)
{
  mmd.sculpt_color_grids = MEM_new_array_zeroed<MultiresColorGrid>(2, "test colors");
  mmd.sculpt_color_grids_num = 2;
  mmd.sculpt_color_level = 1;
  const MDisps *original = layer->disps;
  const int map[] = {2, 0};
  EXPECT_FALSE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(layer->disps, original);
  EXPECT_EQ(layer->grids_num, 3);
}

TEST_F(MultiresLayerRemap, InvalidFaceSetStorageDoesNotPublishLayers)
{
  mmd.sculpt_face_set_grids = MEM_new_array_zeroed<MultiresFaceSetGrid>(3, "test face sets");
  mmd.sculpt_face_set_grids_num = 3;
  mmd.sculpt_face_set_level = 2;
  const MDisps *original = layer->disps;
  const int map[] = {2, 0};
  EXPECT_FALSE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(layer->disps, original);
}

TEST_F(MultiresLayerRemap, NewFaceSetsStayUniformWithoutDensePayload)
{
  mmd.sculpt_face_set_grids = MEM_new_array_zeroed<MultiresFaceSetGrid>(3, "test face sets");
  mmd.sculpt_face_set_grids_num = 3;
  mmd.sculpt_face_set_level = 1;
  mmd.sculpt_face_set_grids[2].level = 1;
  mmd.sculpt_face_set_grids[2].uniform_id = 9;
  const int map[] = {2, -1};
  ASSERT_TRUE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(mmd.sculpt_face_set_grids[0].uniform_id, 9);
  EXPECT_EQ(mmd.sculpt_face_set_grids[1].ids, nullptr);
  EXPECT_EQ(mmd.sculpt_face_set_grids[1].totid, 0);
  EXPECT_EQ(mmd.sculpt_face_set_grids[1].level, 1);
  EXPECT_EQ(mmd.sculpt_face_set_grids[1].uniform_id, 1);
}

TEST_F(MultiresLayerRemap, SuccessfulRemapDisablesRecording)
{
  mmd.sculpt_layer_recording = 1;
  const int map[] = {1, -1, 2};
  ASSERT_TRUE(remap_surviving_corners(mmd, 3, map));
  EXPECT_EQ(mmd.sculpt_layer_recording, 0);
}

}  // namespace blender::bke::multires_layers::tests
