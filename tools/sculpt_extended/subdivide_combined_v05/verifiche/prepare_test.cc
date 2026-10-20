#include "../payload/multires_subdivide_scratch_v04.hh"
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <thread>
#include <vector>

struct Grid {
  int totdisp;
  float (*disps)[3];
};

// Frozen scalar loop from the verified V03 preparation path.
bool reference(Grid &grid, const Grid *layer)
{
  for (int i = 0; i < grid.totdisp; ++i) {
    for (int a = 0; a < 3; ++a) {
      if (layer && layer->disps) grid.disps[i][a] += layer->disps[i][a];
      if (!std::isfinite(grid.disps[i][a])) return false;
    }
  }
  return true;
}

int main()
{
  using namespace blender::bke::multires_layers::subdivide_scratch_v04;
  using Value = std::array<float, 3>;
  std::mt19937 random(401);
  std::uniform_real_distribution<float> values(-100.0f, 100.0f);
  for (int area : {0, 1, 9, 81, 1089, 4225}) {
    const int count = 37;
    std::vector<std::vector<Value>> source(count), layers(count), expected(count), result(count);
    for (int g = 0; g < count; ++g) {
      source[g].resize(area); layers[g].resize(area);
      for (int i = 0; i < area; ++i) for (int a = 0; a < 3; ++a) {
        source[g][i][a] = values(random); layers[g][i][a] = values(random);
      }
      if (area) { source[g][0] = {-0.0f, 0.0f, std::numeric_limits<float>::denorm_min()}; }
    }
    const auto source_copy = source, layers_copy = layers;
    expected = result = source;
    for (int g = 0; g < count; ++g) {
      Grid work{area, reinterpret_cast<float (*)[3]>(expected[g].data())};
      Grid layer{area, g % 3 ? reinterpret_cast<float (*)[3]>(layers[g].data()) : nullptr};
      assert(reference(work, g % 5 ? &layer : nullptr));
    }
    std::atomic<int> next{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) workers.emplace_back([&] {
      for (int g = next++; g < count; g = next++) {
        Grid work{area, reinterpret_cast<float (*)[3]>(result[g].data())};
        Grid layer{area, g % 3 ? reinterpret_cast<float (*)[3]>(layers[g].data()) : nullptr};
        assert(prepare_grid(work, g % 5 ? &layer : nullptr));
      }
    });
    for (auto &worker : workers) worker.join();
    for (int g = 0; g < count; ++g) {
      if (area) {
        assert(std::memcmp(expected[g].data(), result[g].data(), area * sizeof(Value)) == 0);
        assert(std::memcmp(source[g].data(), source_copy[g].data(), area * sizeof(Value)) == 0);
        assert(std::memcmp(layers[g].data(), layers_copy[g].data(), area * sizeof(Value)) == 0);
      }
    }
  }
  for (float bad : {std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN()}) {
    float value[1][3] = {{0, bad, 0}};
    Grid grid{1, value}; assert(!prepare_grid(grid, static_cast<Grid *>(nullptr)));
    float base[1][3] = {{0, 0, 0}};
    Grid clean{1, base}; assert(!prepare_grid(clean, &grid));
  }
  float max[1][3] = {{std::numeric_limits<float>::max(), 0, 0}};
  float delta[1][3] = {{std::numeric_limits<float>::max(), 0, 0}};
  Grid work{1, max}, layer{1, delta}; assert(!prepare_grid(work, &layer));
  std::cout << "PASS V04 production preparation: bit-exact V03, eight workers, null/empty grids, signed zero/subnormals, nonfinite/overflow rejection, immutable source\n";
}
