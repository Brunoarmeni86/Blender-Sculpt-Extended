#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static constexpr int MULTIRES_SCULPT_LAYER_VISIBLE = 1;
struct MDisps { float (*disps)[3] = nullptr; int totdisp = 0; };
struct MultiresSculptLayer {
  int flag=0;
  float strength=1.0f;
  int base_verts_num=0;
  float (*base_deltas)[3]=nullptr;
  std::vector<MDisps> disps;
};
struct MultiresModifierData {
  int totlvl = 1;
  int sculpt_layer_recording=0;
  std::vector<MultiresSculptLayer> sculpt_layers;
};
struct FakeId{};
struct Mesh {
  int corners_num=1;
  FakeId id;
  std::vector<MDisps> corner_data;
  bool changed = false;
  void tag_positions_changed(){changed=true;}
};
int ID_REAL_USERS(const FakeId&){return 1;}
bool compatible(const MultiresModifierData&, const Mesh&){return true;}
bool is_effective(const MultiresSculptLayer &layer) {
  return (layer.flag & MULTIRES_SCULPT_LAYER_VISIBLE) != 0 && std::isfinite(layer.strength) &&
         std::abs(layer.strength) >= 1e-6f;
}
void weight_hot_delta_cache_invalidate(){}
void* CustomData_get_layer_for_write(std::vector<MDisps> *corners, int, int){return corners->data();}
constexpr int CD_MDISPS=1;

bool bake_visible_to_base(MultiresModifierData &mmd, Mesh &mesh)
{
  /* BAKE_VISIBLE_TO_BASE_V2 */
  if (!compatible(mmd, mesh) || ID_REAL_USERS(mesh.id) > 1 || mmd.totlvl <= 0) {
    return false;
  }

  int baked_layer_count = 0;
  for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!is_effective(layer)) {
      continue;
    }
    baked_layer_count++;
    if (layer.base_deltas) {
      for (int vertex = 0; vertex < layer.base_verts_num; vertex++) {
        for (int axis = 0; axis < 3; axis++) {
          if (layer.base_deltas[vertex][axis] != 0.0f) {
            std::printf("[LAYER_BAKE] mode=REJECT reason=VISIBLE_L0_CAGE_DEFORMATION\n");
            return false;
          }
        }
      }
    }
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      if (!layer.disps[corner].disps) {
        return false;
      }
    }
  }
  if (baked_layer_count == 0) {
    return false;
  }

  MDisps *base = static_cast<MDisps *>(
      CustomData_get_layer_for_write(&mesh.corner_data, CD_MDISPS, mesh.corners_num));

  for (int pass = 0; pass < 2; pass++) {
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      for (int i = 0; i < base[corner].totdisp; i++) {
        for (int axis = 0; axis < 3; axis++) {
          float value = base[corner].disps[i][axis];
          if (!std::isfinite(value)) {
            return false;
          }
          for (const MultiresSculptLayer &layer : mmd.sculpt_layers) {
            if (!is_effective(layer)) {
              continue;
            }
            const float displacement = layer.disps[corner].disps[i][axis];
            if (!std::isfinite(displacement)) {
              return false;
            }
            value += displacement * layer.strength;
            if (!std::isfinite(value)) {
              return false;
            }
          }
          if (pass == 1) {
            base[corner].disps[i][axis] = value;
          }
        }
      }
    }
  }

  for (MultiresSculptLayer &layer : mmd.sculpt_layers) {
    if (!is_effective(layer)) {
      continue;
    }
    for (int corner = 0; corner < mesh.corners_num; corner++) {
      memset(layer.disps[corner].disps,
             0,
             sizeof(float[3]) * layer.disps[corner].totdisp);
    }
    if (layer.base_deltas) {
      memset(layer.base_deltas, 0, sizeof(float[3]) * layer.base_verts_num);
    }
  }

  weight_hot_delta_cache_invalidate();
  mesh.tag_positions_changed();
  mmd.sculpt_layer_recording = 0;
  std::printf("[LAYER_BAKE] mode=VISIBLE_TO_BASE layers=%d hidden_preserved=1 l0_reshape=0\n",
              baked_layer_count);
  return true;
}

struct OwnedGrid {
  std::vector<float> values;
  MDisps disp;
  explicit OwnedGrid(float x):values{x,0.f,0.f}{disp={reinterpret_cast<float(*)[3]>(values.data()),1};}
};
static bool eq(float x,float y){return std::abs(x-y)<1e-5f;}
int main() {
  OwnedGrid baseGrid(3), visibleGrid(2), negativeGrid(4), hiddenGrid(10), zeroGrid(8);
  Mesh mesh; mesh.corner_data={baseGrid.disp};
  MultiresModifierData mmd;
  mmd.sculpt_layers.resize(4);
  auto add=[&](int i, OwnedGrid& owned,int flag,float strength){
    mmd.sculpt_layers[i].disps={owned.disp};
    mmd.sculpt_layers[i].flag=flag;
    mmd.sculpt_layers[i].strength=strength;
  };
  add(0,visibleGrid,1,1.0f);
  add(1,negativeGrid,1,-0.5f);
  add(2,hiddenGrid,0,1.0f);
  add(3,zeroGrid,1,0.0f);
  const float before=3+2-4*0.5f;
  assert(eq(before,3.f));
  assert(bake_visible_to_base(mmd,mesh));
  assert(eq(baseGrid.values[0],before));
  assert(eq(visibleGrid.values[0],0));
  assert(eq(negativeGrid.values[0],0));
  assert(eq(hiddenGrid.values[0],10));
  assert(eq(zeroGrid.values[0],8));
  assert(mesh.changed);
  assert(eq(baseGrid.values[0]+hiddenGrid.values[0],13));
  std::printf("PASS: visible+negative weight, hidden preservation, zero strength, no double-count\n");

  float L0[1][3]={{0.1f,0,0}};
  mmd.sculpt_layers[0].base_deltas=L0;
  mmd.sculpt_layers[0].base_verts_num=1;
  visibleGrid.values[0]=2;
  const float previous=baseGrid.values[0];
  assert(!bake_visible_to_base(mmd,mesh));
  assert(eq(baseGrid.values[0],previous));
  assert(eq(visibleGrid.values[0],2));
  std::printf("PASS: visible L0 cage deformation refused atomically\n");

  L0[0][0]=0.0f;
  visibleGrid.values[0]=std::numeric_limits<float>::quiet_NaN();
  assert(!bake_visible_to_base(mmd,mesh));
  assert(eq(baseGrid.values[0],previous));
  assert(std::isnan(visibleGrid.values[0]));
  std::printf("PASS: invalid grid data refused atomically\n");
  return 0;
}
