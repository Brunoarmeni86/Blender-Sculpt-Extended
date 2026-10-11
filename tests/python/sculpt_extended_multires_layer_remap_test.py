# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import re
import subprocess
import tempfile
import os
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description='Compile the actual remap function against minimal storage adapters; not a Blender integration test.')
parser.add_argument('--source', type=Path)
args = parser.parse_args()
source = (args.source or root / 'source/blender/blenkernel/intern/multires_sculpt_layers.cc').read_text()
tests = (root / 'source/blender/blenkernel/intern/multires_sculpt_layers_test.cc').read_text()

def function(name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    opening = source.index('{', start)
    depth = 1
    i = opening + 1
    while depth:
        depth += (source[i] == '{') - (source[i] == '}')
        i += 1
    return source[start:i]

stub = r'''
#include <vector>
#include <deque>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstddef>
#include <type_traits>
template<class T> using Array = std::vector<T>;
template<class T> struct Span {
 const T *p; size_t n;
 template<size_t N> Span(const T (&a)[N]):p(a),n(N){}
 size_t size() const {return n;} bool is_empty()const{return n==0;}
 const T* begin()const{return p;} const T* end()const{return p+n;}
 const T& operator[](size_t i)const{return p[i];}
 struct Range {size_t n; struct It {size_t i; size_t operator*()const{return i;}
 It& operator++(){++i;return *this;} bool operator!=(It b)const{return i!=b.i;}};
 It begin()const{return {0};} It end()const{return {n};}};
 Range index_range()const{return {n};}
};
template<class T> T* MEM_new_array_zeroed(size_t n,const char*){
 static_assert(std::is_trivial_v<T>, "For non-trivial types, MEM_new must be used.");
 return new T[n]();}
template<class T> T* MEM_new_array(size_t n,const char*){return new T[n];}
template<class T> void MEM_delete(T*p){delete[] p;}
// Only needed to compile the original implementation for the sparse-grid regression.
// The baseline aborts on that test before it reaches payload duplication.
template<class T> T* MEM_dupalloc(T*){return nullptr;}
#define MEM_SAFE_DELETE(p) do {MEM_delete(p);p=nullptr;} while(0)
struct MDisps {float (*disps)[3]; unsigned int *hidden; int level,totdisp;};
struct MultiresSculptLayer {MDisps*disps=nullptr; int grids_num=0,level=1;
 float (*base_deltas)[3]=nullptr; int base_verts_num=0;};
struct MultiresColorGrid {float (*colors)[4];int totcolor,level;};
struct MultiresFaceSetGrid {int *ids=nullptr;int totid=0,level=0,uniform_id=1;};
struct MultiresModifierData {int totlvl=1;std::deque<MultiresSculptLayer> sculpt_layers;
 MultiresColorGrid *sculpt_color_grids=nullptr;int sculpt_color_grids_num=0,sculpt_color_level=0;
 MultiresFaceSetGrid *sculpt_face_set_grids=nullptr;int sculpt_face_set_grids_num=0,sculpt_face_set_level=0;
 int sculpt_layer_recording=0;};
namespace subdiv {int grid_size_from_level(int level){return (1<<level)+0;}}
void weight_hot_delta_cache_invalidate(){}
void polypaint_clear(MultiresModifierData&m){for(int i=0;i<m.sculpt_color_grids_num;i++)
 MEM_delete(m.sculpt_color_grids[i].colors); MEM_SAFE_DELETE(m.sculpt_color_grids);
 m.sculpt_color_grids_num=0;m.sculpt_color_level=0;}
void faceset_clear(MultiresModifierData&m){for(int i=0;i<m.sculpt_face_set_grids_num;i++)
 MEM_delete(m.sculpt_face_set_grids[i].ids); MEM_SAFE_DELETE(m.sculpt_face_set_grids);
 m.sculpt_face_set_grids_num=0;m.sculpt_face_set_level=0;}
'''
support = r'''
MultiresSculptLayer *add(MultiresModifierData&m,const char*,int level,int count){
 m.sculpt_layers.emplace_back();auto &l=m.sculpt_layers.back();l.level=level;l.grids_num=count;
 l.disps=MEM_new_array_zeroed<MDisps>(count,"");
 for(int i=0;i<count;i++){l.disps[i].level=level;l.disps[i].totdisp=4;}
 return &l;}
float (*grid_ensure(MultiresSculptLayer&l,int i))[3]{
 if(!l.disps[i].disps)l.disps[i].disps=MEM_new_array_zeroed<float[3]>(4,"");
 return l.disps[i].disps;}
bool base_deltas_ensure(MultiresSculptLayer&l,int n){l.base_deltas=MEM_new_array_zeroed<float[3]>(n,"");l.base_verts_num=n;return true;}
void clear(MultiresModifierData&m){for(auto&l:m.sculpt_layers)free_layer(l);m.sculpt_layers.clear();}
namespace testing {class Test {public:virtual void SetUp(){};virtual void TearDown(){};virtual ~Test(){};};}
std::vector<void(*)()> runners;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();}}while(0)
#define ASSERT_TRUE(x) CHECK(x)
#define EXPECT_FALSE(x) CHECK(!(x))
#define EXPECT_EQ(x,y) CHECK((x)==(y))
#define ASSERT_NE(x,y) CHECK((x)!=(y))
#define EXPECT_FLOAT_EQ(x,y) CHECK((x)==(y))
#define TEST_F(F,N) class F##N:public F {public:void Body();void Run(){SetUp();Body();TearDown();}}; \
void run##N(){F##N t;t.Run();std::puts(#N);} \
static bool register##N=[](){runners.push_back(run##N);return true;}();void F##N::Body()
'''
tests = re.sub(r'^#include.*$', '', tests, flags=re.M)
tests = tests.replace('namespace blender::bke::multires_layers::tests {', '')
tests = tests.replace('}  // namespace blender::bke::multires_layers::tests', '')
temp = tempfile.TemporaryDirectory(prefix='multires-layer-remap-')
out = Path(temp.name) / 'remap_harness.cc'
base_tests = r'''
void base_grid_tests(){
 MDisps valid{}; valid.level=1;valid.totdisp=4;
 valid.disps=MEM_new_array_zeroed<float[3]>(4,"");valid.disps[0][0]=2.5f;
 valid.hidden=MEM_new_array_zeroed<unsigned int>(1,"");valid.hidden[0]=5;
 auto *payload=valid.disps; auto *hidden=valid.hidden;
 CHECK(!initialize_missing_base_grid(valid,1,4));
 CHECK(valid.disps==payload && valid.hidden==hidden && valid.disps[0][0]==2.5f);
 MEM_delete(valid.disps);MEM_delete(valid.hidden);
 MDisps empty{};
 CHECK(initialize_missing_base_grid(empty,1,4));
 CHECK(empty.level==1 && empty.totdisp==4 && empty.disps && !empty.hidden);
 for(int i=0;i<4;i++)for(int a=0;a<3;a++)CHECK(empty.disps[i][a]==0);
 MEM_delete(empty.disps);
 MDisps wrong_level{};wrong_level.level=2;wrong_level.totdisp=9;
 wrong_level.disps=MEM_new_array_zeroed<float[3]>(9,"");
 CHECK(initialize_missing_base_grid(wrong_level,1,4));
 CHECK(wrong_level.level==1 && wrong_level.totdisp==4 && wrong_level.disps[0][0]==0);
 MEM_delete(wrong_level.disps);
 MDisps wrong_area{};wrong_area.level=1;wrong_area.totdisp=9;
 wrong_area.disps=MEM_new_array_zeroed<float[3]>(9,"");
 CHECK(initialize_missing_base_grid(wrong_area,1,4));
 CHECK(wrong_area.totdisp==4 && wrong_area.disps[0][0]==0);
 MEM_delete(wrong_area.disps);
 std::puts("BaseInterpolation: 4 cases PASS");
}
'''
has_base_helper = 'static bool initialize_missing_base_grid(' in source
helper = function('static bool initialize_missing_base_grid(') if has_base_helper else ''
out.write_text(stub + function('static void free_layer(') + '\n' + helper + '\n' +
               function('bool remap_surviving_corners(') + support + tests +
               (base_tests if has_base_helper else '') +
               '\nint main(){for(auto run:runners)run();' +
               ('base_grid_tests();' if has_base_helper else '') + '}\n')
subprocess.run(['g++','-std=c++17','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                str(out),'-o',str(out.with_suffix(''))],check=True)
env = os.environ.copy()
# LeakSanitizer cannot enumerate processes in some managed sandboxes. ASan and UBSan remain enabled.
env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
subprocess.run([str(out.with_suffix(''))], check=True, env=env)
