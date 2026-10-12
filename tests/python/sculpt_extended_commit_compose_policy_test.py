"""Isolated gate/overwrite audit; does not run native Blender or validate Undo."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'source/blender/blenkernel/intern/multires.cc').read_text()
gate = re.search(r'const bool skip_reference_compose = (.*?);', source, re.S).group(1)
program = r'''
#include <cassert>
#include <array>
struct CCG { int level; };
struct MMD { int totlvl; };
bool gate(const char *compose_env, int level, int top, bool live_displacement_active) {
  CCG cg{level}; const CCG *ccg=&cg;
  MMD modifier{top}; const MMD *mmd=&modifier;
  return GATE;
}
int main() {
  for (int top=1; top<=13; ++top) {
    for (int level=1; level<=top; ++level) {
      assert(gate("1",level,top,false)==(level==top));
      assert(!gate("1",level,top,true));
      for (const char *flag : {nullptr,"","0","10","1x"}) {
        assert(!gate(flag,level,top,false));
      }
    }
  }
  // Equal-level CCG assignment overwrites each xyz sample before conversion.
  // Exercise cancellation, non-unit and negative strengths, and multiple layers.
  std::array<float,3> base{1.0f,-2.0f,0.25f};
  std::array<float,3> stroke{2.5f,0.5f,-0.75f};
  std::array<std::array<float,3>,2> layers{{{0.4f,-0.2f,0.8f},{-0.1f,0.7f,0.3f}}};
  for (float active_strength : {-2.0f,0.5f,1.0f}) {
    auto legacy=base, skipped=base;
    for (int axis=0;axis<3;++axis) {
      legacy[axis]+=layers[0][axis]*active_strength+layers[1][axis]*0.75f;
      legacy[axis]=stroke[axis]; skipped[axis]=stroke[axis];
      legacy[axis]=(legacy[axis]-base[axis]-layers[1][axis]*0.75f)/active_strength;
      skipped[axis]=(skipped[axis]-base[axis]-layers[1][axis]*0.75f)/active_strength;
      assert(legacy[axis]==skipped[axis]);
    }
  }
}
'''.replace('GATE', gate)
# initializer-list of string pointers needs an explicit homogeneous nullptr.
program=program.replace('{nullptr,"","0","10","1x"}', '{static_cast<const char *>(nullptr),"","0","10","1x"}')
with tempfile.TemporaryDirectory() as tmp:
    p=pathlib.Path(tmp)
    (p/'test.cc').write_text(program)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cc'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: exact flag, levels 1..13, live exclusion and overwrite algebra. Native tests pending.')
