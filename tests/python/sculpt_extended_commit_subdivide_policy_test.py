"""Extracted policy/ownership routing checks; not a native Blender reshape test."""
from pathlib import Path
import subprocess
import tempfile
r=Path(__file__).resolve().parents[2]
s=(r/'source/blender/blenkernel/intern/multires_reshape.cc').read_text()
def extract(name):
 a=s.index(name+'(');a=s.rfind('\n',0,a)+1;b=s.index('{',a);level=1;i=b+1
 while level:
  if s[i]=='{':level+=1
  elif s[i]=='}':level-=1
  i+=1
 return s[a:i]
cpp=r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
struct Mesh{};struct SubdivCCG{int level;};
struct MultiresReshapeContext {struct{int level;}reshape,top;};
int copies=0,frees=0;bool assign_ok=true;
const char *BLI_getenv(const char*n){return std::getenv(n);}
bool multires_reshape_context_create_from_ccg(MultiresReshapeContext*c,SubdivCCG*g,Mesh*,int top){c->reshape.level=g->level;c->top.level=top;return true;}
void multires_ensure_external_read(Mesh*,int){}
void multires_reshape_store_original_grids(MultiresReshapeContext*){copies++;}
void multires_reshape_ensure_grids(Mesh*,int){}
bool multires_reshape_assign_final_coords_from_ccg(MultiresReshapeContext*,SubdivCCG*){return assign_ok;}
void multires_reshape_context_free(MultiresReshapeContext*){frees++;}
void multires_reshape_smooth_object_grids_with_details(MultiresReshapeContext*){}
void multires_reshape_object_grids_to_tangent_displacement(MultiresReshapeContext*){}
struct MDisps{float (*disps)[3]=nullptr;void *hidden=nullptr;int level=2,totdisp=9;};
struct MultiresSculptLayer{int level=2,grids_num=2;MDisps *disps;float (*base_deltas)[3]=nullptr;int base_verts_num=0;};
namespace bke::subdiv{int grid_size_from_level(int n){return (1<<(n-1))+1;}}
#define BLI_assert assert
'''+extract('multiresModifier_reshapeFromCCG')+'\n'+extract('multires_layer_can_keep_empty_subdivide')+'\n'+extract('multires_publish_empty_subdivided_layer')+r'''
int main(){
 Mesh mesh;SubdivCCG ccg{3};
 setenv("BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS","0",1);
 assert(multiresModifier_reshapeFromCCG(3,&mesh,&ccg));assert(copies==1&&frees==1);
 setenv("BLENDER_SCULPT_COMMIT_SKIP_ORIGINALS","1",1);
 assert(multiresModifier_reshapeFromCCG(3,&mesh,&ccg));assert(copies==1&&frees==2);
 ccg.level=2;assert(multiresModifier_reshapeFromCCG(3,&mesh,&ccg));assert(copies==2);
 ccg.level=3;assign_ok=false;assert(!multiresModifier_reshapeFromCCG(3,&mesh,&ccg));assert(frees==4);
 MDisps grids[2]{};MultiresSculptLayer layer{2,2,grids};
 assert(multires_layer_can_keep_empty_subdivide(layer));
 float values[9][3]{};grids[1].disps=values;assert(!multires_layer_can_keep_empty_subdivide(layer));grids[1].disps=nullptr;
 grids[0].hidden=&mesh;assert(!multires_layer_can_keep_empty_subdivide(layer));grids[0].hidden=nullptr;
 float cage[1][3]{{1,0,0}};layer.base_deltas=cage;layer.base_verts_num=1;assert(!multires_layer_can_keep_empty_subdivide(layer));layer.base_deltas=nullptr;
 multires_publish_empty_subdivided_layer(layer,3);
 assert(layer.level==3&&grids[0].level==3&&grids[1].totdisp==25&&grids[0].disps==nullptr);
 std::puts("PASS: equal/lower-level reshape gates, cleanup, empty/partial/hidden/L0 guards and sparse metadata publication.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cc';p.write_text(cpp);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
