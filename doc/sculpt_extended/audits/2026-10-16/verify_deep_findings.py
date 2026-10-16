#!/usr/bin/env python3
"""Deeper audit: real extracted functions, minimal adapters, no Blender build.

Run alongside verify_findings.py, optionally passing the audited checkout root.
Expected sanitizer faults are isolated in child processes and checked explicitly.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import re
from verify_findings import extract, PREAMBLE

HERE = Path(__file__).resolve().parent
SRC = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else HERE
LAYERS = SRC/'source/blender/blenkernel/intern/multires_sculpt_layers.cc'
MULTI = SRC/'source/blender/blenkernel/intern/multires.cc'
UNDO = SRC/'source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc'

ADAPTERS = r'''
template<class T> struct MutableSpan {
  T* ptr; int count;
  T* data()const{return ptr;}int size()const{return count;}
  T* begin()const{return ptr;}T* end()const{return ptr+count;}
  template<class A> void copy_from(const A& values){std::copy(values.begin(),values.end(),ptr);}
};
struct ID { int users=1; };
#define ID_REAL_USERS(id) ((id)->users)
struct CustomData { MDisps* disps=nullptr; };
constexpr int CD_MDISPS=0;
const void* CustomData_get_layer(const CustomData* c,int){return c->disps;}
void* CustomData_get_layer_for_write(CustomData* c,int,int){return c->disps;}
struct Mesh {
  ID id;int corners_num=1,verts_num=1;CustomData corner_data;
  Array<float3> positions{float3(0)};
  const Array<float3>& vert_positions()const{return positions;}
  MutableSpan<float3> vert_positions_for_write(){return {positions.data(),int(positions.size())};}
  void tag_positions_changed(){}
};
struct Runtime { struct SculptSession* sculpt_session=nullptr; };
struct Object { Mesh* data=nullptr;Runtime* runtime=nullptr; };
Mesh* BKE_mesh_from_object(Object* o){return o->data;}
template<class T> T id_cast(Mesh* p){return p;}
void mul_v3_fl(float v[3],float s){for(int i=0;i<3;i++)v[i]*=s;}
struct SculptSession { MultiresModifierData* multires_modifier=nullptr; };
struct Grids {std::vector<int> v;size_t size()const{return v.size();}auto begin()const{return v.begin();}auto end()const{return v.end();}};
struct SavedIDs {std::vector<int> v;size_t size()const{return v.size();}int&operator[](size_t i){return v[i];}};
struct RestoreNode {Grids grids;SavedIDs grid_face_sets;int grid_face_set_side=2;};
#define PRF_scope(x) ((void)0)
namespace ProfileCategory {constexpr int Editor=0;}
namespace blender::bke::subdiv {int grid_size_from_level(int l){return (1<<(l-1))+1;}}
namespace bke=blender::bke;
'''

TESTS = r'''
static MultiresSculptLayer* layer(MultiresModifierData& m,bool dense){
  auto *p=(MultiresSculptLayer*)malloc(sizeof(MultiresSculptLayer));new(p)MultiresSculptLayer{};
  p->disps=(MDisps*)malloc(sizeof(MDisps));new(p->disps)MDisps{};
  if(dense){p->disps[0].disps=MEM_new_array_zeroed<float[3]>(4,"test");p->disps[0].disps[0][0]=1;}
  m.sculpt_layers.first=m.sculpt_layers.last=p;return p;
}
int main(int argc,char**argv){
  assert(argc==2);std::string mode=argv[1];
  using namespace blender::bke::multires_layers;
  if(mode=="bake_sparse" || mode=="bake_dense"){
    MultiresModifierData m;Mesh mesh;MDisps native{};native.disps=MEM_new_array_zeroed<float[3]>(4,"native");
    mesh.corner_data.disps=&native;auto *p=layer(m,mode=="bake_dense");
    assert(compatible(m,mesh));
    bool ok=bake_visible_to_base(m,mesh);
    assert(ok&&native.disps[0][0]==1&&p->disps[0].disps[0][0]==0);
    std::puts("D1 dense bake control: contribution preserved and cleared");
    free_layer(*p);free(p);free(native.disps);
  }
  else if(mode=="scale"){
    MultiresModifierData m;Mesh mesh;Object object{&mesh,nullptr};
    MDisps native{};native.disps=MEM_new_array_zeroed<float[3]>(4,"native");native.disps[0][0]=1;
    mesh.corner_data.disps=&native;auto *p=layer(m,true);
    p->base_verts_num=1;p->base_deltas=MEM_new_array_zeroed<float[3]>(1,"cage");p->base_deltas[0][0]=1;
    multires_apply_uniform_scale(&object,2);
    assert(native.disps[0][0]==2&&p->disps[0].disps[0][0]==1&&p->base_deltas[0][0]==1);
    std::puts("D2 uniform scale x2: native 1->2, HR layer 1->1, L0 layer 1->1");
    std::puts("D2 planar normalized-frame oracle: world detail before=2*(1+1)=4, after=2+1=3");
    free_layer(*p);free(p);free(native.disps);
  }
  else if(mode=="faceset_uniform" || mode=="faceset_short"){
    MultiresModifierData m;SculptSession ss{&m};Runtime runtime{&ss};Object o{nullptr,&runtime};
    m.sculpt_face_set_grids_num=1;m.sculpt_face_set_level=1;
    m.sculpt_face_set_grids=(MultiresFaceSetGrid*)malloc(sizeof(MultiresFaceSetGrid));
    new(m.sculpt_face_set_grids)MultiresFaceSetGrid{};
    RestoreNode n;n.grids.v={0};n.grid_face_sets.v={1,1,1,1};
    if(mode=="faceset_short"){
      m.sculpt_face_set_grids[0].ids=MEM_new_array_zeroed<int>(1,"short");
      m.sculpt_face_set_grids[0].totid=1;m.sculpt_face_set_grids[0].level=1;
    }
    bool changed=restore_face_sets_grids(o,n);
    assert(!changed&&m.sculpt_face_set_grids[0].ids!=nullptr&&m.sculpt_face_set_grids[0].totid==4);
    std::puts("D3 unchanged uniform Face Set undo: modified=false, dense allocation created");
    faceset_clear(m);
  }
}
'''

ENCODE_ADAPTERS = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
struct float3 {float v[3];};
enum class Type {None,Position,Color,DyntopoEnd};
struct bContext{};
struct Main {bool is_memfile_undo_flush_needed=false;};
struct UndoStep {void* type=nullptr;size_t data_size=0;char name[64]="test";bool use_memfile_step=false,is_applied=false;};
void* BKE_UNDOSYS_TYPE_SCULPT=(void*)1;
struct SculptLayerUndoFrameItem {std::string name;};
struct SculptLayerUndoMetadata {std::string name;};
struct PendingCompression {size_t* size;void ensure_compression_complete(){*size=1048576;}};
struct StepData {
  size_t undo_size=0;Type type=Type::Position;
  std::vector<float3> base_layer_deltas;
  struct {int grids_num=0;}grids;
  std::vector<int>nodes;
  std::vector<SculptLayerUndoFrameItem> layer_frame;
  std::string layer_frame_modifier;
  std::vector<SculptLayerUndoMetadata> layer_metadata;
  std::string layer_metadata_modifier;
  std::unique_ptr<PendingCompression> position_step_storage;
};
struct SculptUndoStep {UndoStep step;StepData data;};
'''
ENCODE_TEST = r'''
int main(){
  Main main;SculptUndoStep us;us.step.type=BKE_UNDOSYS_TYPE_SCULPT;
  us.data.position_step_storage=std::make_unique<PendingCompression>();
  us.data.position_step_storage->size=&us.data.undo_size;
  assert(step_encode(nullptr,&main,&us.step));
  const size_t before=us.step.data_size;
  size_t complete=step_memory_size_get(&us.step);
  assert(complete==1048576&&before==0&&us.step.data_size==0);
  std::puts("D4 delayed-compression schedule: completed undo_size=1 MiB, step.data_size remains 0");
}
'''

def main():
    layers, multi, undo = LAYERS.read_text(), MULTI.read_text(), UNDO.read_text()
    preamble=PREAMBLE.replace('namespace blender::bke::multires_layers {','')
    preamble=preamble.replace('namespace subdiv { int grid_size_from_level(int level){return (1<<(level-1))+1;} }','')
    preamble=preamble.replace('void weight_hot_delta_cache_invalidate(const MultiresSculptLayer* =nullptr){}','')
    preamble=preamble.replace('bool merge_down(MultiresModifierData&);','')
    preamble=preamble.replace('List sculpt_layers; int','int totlvl=1;List sculpt_layers; int')
    # Array-to-span conversion is an API adapter; function bodies stay unchanged.
    preamble=preamble.replace('template<class T> struct Array :', 'template<class T> struct MutableSpan;\ntemplate<class T> struct Array :')
    preamble=preamble.replace('using std::vector<T>::vector; };','using std::vector<T>::vector; operator MutableSpan<T>(){return {this->data(),int(this->size())};} };')
    function_specs=[('layers','bool is_effective('),('layers','bool base_deltas_ensure('),
       ('layers','bool base_deltas_apply('),('layers','bool compatible('),
       ('layers','bool bake_visible_to_base('),('layers','static void free_layer('),
       ('layers','void faceset_clear('),('layers','int *faceset_grid_ensure_dense(')]
    bodies=[extract(layers,sig) for _,sig in function_specs]
    # restore uses production Node; alias only its type in the adapter environment.
    restore=extract(undo,'static bool restore_face_sets_grids(')
    scale=extract(multi,'static void multires_apply_uniform_scale(')
    code=preamble+'\n#include <string>\n'+ADAPTERS+'\n'
    code+='namespace blender::bke::multires_layers {\nnamespace subdiv {int grid_size_from_level(int l){return (1<<(l-1))+1;}}\n'
    code+='void weight_hot_delta_cache_invalidate(const MultiresSculptLayer* =nullptr){}\n'
    code+='\n\n'.join(bodies)+'\n}\n'
    code+='\n'+scale+'\n#define Node RestoreNode\n'+restore+'\n#undef Node\n'+TESTS
    build=HERE/'build-deep';build.mkdir(exist_ok=True)
    cpp=build/'deep.cc';cpp.write_text(code)
    flags=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(SRC)]
    subprocess.run(flags+[str(cpp),'-o',str(build/'deep')],check=True)
    encoded=build/'encode.cc'
    encode_funcs=[extract(undo,'static bool step_encode('),extract(undo,'size_t step_memory_size_get(')]
    encoded.write_text(ENCODE_ADAPTERS+'\n'.join(encode_funcs)+ENCODE_TEST)
    subprocess.run(flags+[str(encoded),'-o',str(build/'encode')],check=True)
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1')
    lines=[]
    for case in ['bake_dense','scale','faceset_uniform']:
        r=subprocess.run([str(build/'deep'),case],env=env,text=True,capture_output=True)
        if r.returncode: raise RuntimeError(r.stdout+r.stderr)
        lines.append(r.stdout.strip())
    for case,expected in [('bake_sparse','null pointer'),('faceset_short','heap-buffer-overflow')]:
        r=subprocess.run([str(build/'deep'),case],env=env,text=True,capture_output=True)
        (HERE/f'deep-{case}-sanitizer.txt').write_text(r.stdout+r.stderr)
        assert r.returncode!=0 and expected in r.stderr,(case,r.returncode,r.stderr)
        lines.append(f'D5 {case}: expected sanitizer failure reproduced ({expected})')
    r=subprocess.run([str(build/'encode')],env=env,text=True,capture_output=True,check=True)
    lines.append(r.stdout.strip())
    threshold=re.search(r'const int dense_sample_threshold = [^;]+;',multi).group(0)
    threshold_cpp=build/'threshold.cc'
    threshold_cpp.write_text('#include <cstdlib>\n#include <cstdio>\nint main(int argc,char**argv){if(argc!=2)return 2;const int level=std::atoi(argv[1]);const int side=(1<<(level-1))+1;const int area=side*side;'+threshold+'std::printf("level=%d threshold=%d\\n",level,dense_sample_threshold);}\n')
    subprocess.run(flags+[str(threshold_cpp),'-o',str(build/'threshold')],check=True)
    r=subprocess.run([str(build/'threshold'),'11'],env=env,text=True,capture_output=True,check=True)
    lines.append('D6 dense-threshold control: '+r.stdout.strip())
    r=subprocess.run([str(build/'threshold'),'12'],env=env,text=True,capture_output=True)
    assert r.returncode!=0 and 'signed integer overflow' in r.stderr,r.stderr
    (HERE/'deep-threshold-sanitizer.txt').write_text(r.stdout+r.stderr)
    lines.append('D6 dense-threshold level 12: expected UBSan signed integer overflow')
    result='\n'.join(lines)+'\n'
    (HERE/'deep-verification-results.txt').write_text(result)
    functions=dict(zip([sig for _,sig in function_specs],bodies))
    functions.update({'static void multires_apply_uniform_scale(':scale,'static bool restore_face_sets_grids(':restore,
                      'static bool step_encode(':encode_funcs[0],'size_t step_memory_size_get(':encode_funcs[1],
                      'dense_sample_threshold expression':threshold})
    manifest={'audited_reference_commit':'01194d089eb5b324b125d0ef6ebaf877dd13f326',
      'scope':'Real function bodies; API/storage adapters; delayed worker completion simulated; not Blender runtime',
      'functions':{s:hashlib.sha256(v.encode()).hexdigest() for s,v in functions.items()},
      'sanitizers':'ASan+UBSan; detect_leaks=0 for executor /proc restriction',
      'checks':'8 child cases: 5 successful observations/controls and 3 expected sanitizer failures'}
    (HERE/'deep-verification-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(result,end='')

if __name__=='__main__': main()
