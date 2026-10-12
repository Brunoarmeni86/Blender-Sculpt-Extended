"""Small extracted-C++ check for new budget arithmetic and index invalidation."""
import pathlib
import subprocess
import tempfile
r=pathlib.Path(__file__).resolve().parents[2]
s=(r/'source/blender/blenkernel/intern/multires_sculpt_layers.cc').read_text()
start=s.index('struct LayerPayloadIndex {');end=s.index('bool is_effective(',start)
helpers=s[start:end]
cpp=r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "multires_layer_cache_budget.hh"
template<class T>struct Vector:std::vector<T>{void append(T a){this->push_back(a);}void extend(const Vector<T>&a){this->insert(this->end(),a.begin(),a.end());}const Vector<T>&as_span()const{return *this;}};
struct MDisps {float (*disps)[3]=nullptr;int level=0,totdisp=0;};
struct MultiresSculptLayer {MDisps *disps=nullptr;int level=0,grids_num=0;};
namespace subdiv {int grid_size_from_level(int level){return (1<<(level-1))+1;}}
const char *BLI_getenv(const char *name){return std::getenv(name);}
namespace blender::bke::multires_layers {
'''+helpers+r'''
}
int main(){
 using namespace blender::bke::multires_layers;
 using namespace cache_budget;
 assert(frame_capacity(8192,289,1000,0,48)==0);
 assert(frame_capacity(8192,289,1000,-1,48)==8192);
 assert(frame_capacity(8192,289,2,512LL*1024*1024,48)==2);
 const long long bytes=1000LL*4+(289LL*48+5)*10;
 assert(frame_capacity(8192,289,1000,bytes*2,48)==10);
 assert(!weight_fits(10,289,1000,0));
 const long long weight=10LL*289*12+1000LL*4+10LL*4;
 assert(weight_fits(10,289,1000,weight*2));assert(!weight_fits(10,289,1000,weight*2-2));
 assert(weight_fits(100000,16785409,100000,-1));
 float payload[4][3]{};MDisps grids[4]{};
 for(auto &g:grids){g.level=1;g.totdisp=4;}
 grids[1].disps=payload;MultiresSculptLayer a{grids,1,4};Vector<int> out;
 setenv("BLENDER_SCULPT_LAYER_SUPPORT_CACHE","1",1);
 assert(payload_grids(a,4,out)&&out.size()==1&&out[0]==1);
 assert(payload_grids(a,4,out)&&out.size()==1);
 grids[3].disps=payload;payload_index_invalidate(&a);
 assert(payload_grids(a,4,out)&&out.size()==2&&out[1]==3);
 grids[1].disps=nullptr;payload_index_invalidate(&a);
 assert(payload_grids(a,4,out)&&out.size()==1&&out[0]==3);
 grids[3].totdisp=5;
 assert(!payload_grids(a,4,out));grids[3].totdisp=4;
 assert(payload_grids(a,4,out));assert(!payload_grids(a,3,out));
 setenv("BLENDER_SCULPT_LAYER_SUPPORT_CACHE","0",1);
 grids[0].disps=payload;
 assert(payload_grids(a,4,out)&&out.size()==2);
 payload_index_invalidate(nullptr);
 std::puts("PASS: budget boundaries, zero/legacy policy, sparse support hits and allocation/free/layout invalidation.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d)/'test.cc';p.write_text(cpp);exe=pathlib.Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(r/'source/blender/blenkernel/intern'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
