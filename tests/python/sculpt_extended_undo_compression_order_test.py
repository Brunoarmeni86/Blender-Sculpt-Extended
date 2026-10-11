"""Check the actual restore prologues with compression pending.

This standalone C++ test checks publication ordering, not Blender integration.
Run with Python and a C++17 compiler available as `c++`.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc"


def prologue(source, name, end):
    start = source.index(name)
    opening = source.index("\n{", start)
    return source[opening + 2:source.index(end, opening)]


def main():
    source = SOURCE.read_text()
    position = prologue(source, "static void restore_position_grids(",
                        "\n  struct LocalData")
    payload = prologue(source, "static bool sculpt_layer_undo_persistent_payload_swap(",
                       "\n  const int area")
    harness = r'''
#include <cassert>
#include <vector>
#define PRF_scope(category) ((void)0)
struct PositionUndoStorage {
  bool sculpt_layer_undo = true;
  int sculpt_layer_grid_area = 9;
  std::vector<int> compressed_indices;
  std::vector<int> compressed_layer_disps;
  bool waited = false;
  bool valid = true;
  void ensure_compression_complete() {
    waited = true;
    compressed_indices = {1, 2};
    compressed_layer_disps = valid ? std::vector<int>{3, 4} : std::vector<int>{3};
  }
};
int restore_position_prologue(PositionUndoStorage &undo_data) {
POSITION
  return nodes_num;
}
bool restore_payload_prologue(PositionUndoStorage &undo_data) {
PAYLOAD
  return true;
}
int main() {
  PositionUndoStorage positions;
  assert(restore_position_prologue(positions) == 2);
  assert(positions.waited);
  PositionUndoStorage payload;
  assert(restore_payload_prologue(payload));
  assert(payload.waited);
  PositionUndoStorage invalid;
  invalid.valid = false;
  assert(!restore_payload_prologue(invalid));
  assert(invalid.waited);
}
'''.replace("POSITION", position).replace("PAYLOAD", payload)
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / "order.cc"
        exe = Path(temp) / "order"
        src.write_text(harness)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: pending positions, pending layer payload, invalid published payload")


if __name__ == "__main__":
    main()
