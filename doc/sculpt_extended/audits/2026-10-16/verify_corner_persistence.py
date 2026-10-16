#!/usr/bin/env python3
"""Actual remap functions, minimal attributes/lists/allocators. No native BMesh IO.
Pass an audited checkout root, or place source/ beside this script.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import re
from verify_findings import extract

HERE=Path(__file__).resolve().parent
SRC=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else HERE

ADAPTERS=r'''
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "source/blender/blenkernel/intern/multires_layer_corner_identity.hh"
struct IndexRange {
 int first_=0,n=0;
 struct It{int i;int operator*()const{return i;}It&operator++(){++i;return *this;}bool operator!=(It o)const{return i!=o.i;}};
 It begin()const{return{first_};}It end()const{return{first_+n};}
 int start()const{return first_;}int size()const{return n;}int last()const{return first_+n-1;}
 int one_after_last()const{return first_+n;}
};
template<class T>struct Span {const T*p=nullptr;int n=0;Span()=default;Span(const T*v,int c):p(v),n(c){}
 const T*data()const{return p;}int size()const{return n;}bool is_empty()const{return n==0;}
 const T*begin()const{return p;}const T*end()const{return n?p+n:p;}const T&operator[](int i)const{return p[i];}
 IndexRange index_range()const{return{0,n};}
};
template<class T>struct Array:std::vector<T>{using std::vector<T>::vector;
 void reinitialize(int n){this->resize(n);}Span<T>as_span()const{return{this->data(),int(this->size())};}
 operator Span<T>()const{return as_span();}IndexRange index_range()const{return{0,int(this->size())};}
};
template<class T>struct MutableSpan {T*p;int n;T&operator[](int i)const{return p[i];}
 template<class A>void copy_from(const A&a){std::copy(a.begin(),a.end(),p);}
};
template<class T>struct AttributeReader {Span<T>varray;bool valid=false;explicit operator bool()const{return valid;}};
template<class T>struct VArraySpan:Span<T>{VArraySpan(Span<T>s):Span<T>(s){};};
template<class T>struct SpanAttributeWriter {MutableSpan<T>span;void finish(){}};
enum class AttrDomain{Corner};
struct Attrs {
 std::unordered_map<std::string,std::vector<int>>*data;int corners;
 template<class T>AttributeReader<T>lookup(const char*name,AttrDomain)const{
  auto it=data->find(name);if(it==data->end())return{};
  return{{it->second.data(),int(it->second.size())},true};
 }
 template<class T>SpanAttributeWriter<T>lookup_or_add_for_write_span(const char*name,AttrDomain){
  auto&v=(*data)[name];v.resize(corners);return{{v.data(),corners}};
 }
};
template<class T>T*MEM_new_array_zeroed(size_t n,const char*){return new T[n]{};}
template<class T>T*MEM_new_array(size_t n,const char*){return new T[n]{};}
template<class T>void MEM_delete(T*p){delete[]p;}
#define MEM_SAFE_DELETE(p) do{delete[](p);(p)=nullptr;}while(0)
struct MDisps {float(*disps)[3]=nullptr;unsigned int*hidden=nullptr;int level=1,totdisp=4;};
struct MultiresColorGrid {float(*colors)[4]=nullptr;int totcolor=0,level=0;};
struct MultiresColorAttribute {MultiresColorGrid*grids=nullptr;int grids_num=0,level=0;};
struct MultiresFaceSetGrid {int*ids=nullptr;int totid=0,level=0,uniform_id=1;};
struct MultiresSculptLayer {MDisps*disps=nullptr;int grids_num=4,level=1;float(*base_deltas)[3]=nullptr;int base_verts_num=0;};
template<class T>struct List:std::vector<T>{using std::vector<T>::vector;void*first=nullptr;bool is_empty()const{return this->empty();}};
struct MultiresModifierData {
 int totlvl=1,sculpt_layer_recording=1;List<MultiresSculptLayer>sculpt_layers;
 List<MultiresColorAttribute>sculpt_color_attributes;
 MultiresColorGrid*sculpt_color_grids=nullptr;int sculpt_color_grids_num=0,sculpt_color_level=0;
 MultiresFaceSetGrid*sculpt_face_set_grids=nullptr;int sculpt_face_set_grids_num=0,sculpt_face_set_level=0;
};
struct FaceOffsets {IndexRange index_range()const{return{0,1};}IndexRange operator[](int)const{return{0,4};}};
using OffsetIndices=FaceOffsets;
struct CornerData {MDisps*grids=nullptr;};
struct Mesh {
 int corners_num=4,high_water=8;CornerData corner_data;
 std::unordered_map<std::string,std::vector<int>>attrs;
 Attrs attributes(){return{&attrs,corners_num};}Attrs attributes_for_write(){return attributes();}
 FaceOffsets faces()const{return{};}
};
constexpr int CD_MDISPS=0;
void*CustomData_get_layer_for_write(CornerData*d,int,int){return d->grids;}
template<int N=0>struct BitVector {
 struct B{bool value=false;void set(){value=true;}};std::vector<B>bits;
 void clear(){bits.clear();}void resize(int n,bool v){bits.resize(n);for(auto&b:bits)b.value=v;}
 B&operator[](int i){return bits[i];}
};
namespace blender::bke::multires_layers {
inline constexpr char corner_id_attribute[]=".sculpt_layer_corner_id_v2";
inline constexpr char corner_prev_attribute[]=".sculpt_layer_corner_prev_v2";
inline constexpr char corner_next_attribute[]=".sculpt_layer_corner_next_v2";
inline constexpr char corner_face_size_attribute[]=".sculpt_layer_face_size_v2";
namespace subdiv {int grid_size_from_level(int l){return(1<<(l-1))+1;}}
void weight_hot_delta_cache_invalidate(const MultiresSculptLayer* =nullptr){}
bool base_remap_after_topology_edit(MultiresModifierData&,Mesh&){return true;}
bool compatible(const MultiresModifierData&,const Mesh&){return true;}
void ensure_corner_ids(MultiresModifierData&,Mesh&){} // Attributes already exist in every case.
int corner_id_counter(Mesh&m){return m.high_water;}
void corner_id_counter_set(Mesh&m,int n){m.high_water=n;}
'''

TESTS=r'''
} // namespace blender::bke::multires_layers
using namespace blender::bke::multires_layers;
struct Fixture {
 MultiresModifierData m;Mesh mesh;
 Fixture(std::vector<int>ids,int stored_corners=4){
  m.sculpt_layers.resize(1);m.sculpt_layers.first=&m.sculpt_layers[0];
  auto&l=m.sculpt_layers[0];l.grids_num=stored_corners;l.disps=MEM_new_array_zeroed<MDisps>(stored_corners,"layers");
  mesh.corner_data.grids=MEM_new_array_zeroed<MDisps>(4,"native");
  for(int i=0;i<stored_corners;i++){
   l.disps[i].disps=MEM_new_array_zeroed<float[3]>(4,"layer payload");l.disps[i].disps[0][0]=float(i+5);
  }
  for(int i=0;i<4;i++){
   mesh.corner_data.grids[i].disps=MEM_new_array_zeroed<float[3]>(4,"base payload");
  }
  mesh.attrs[corner_id_attribute]=ids;mesh.attrs[corner_face_size_attribute]={4,4,4,4};
  for(int i=0;i<4;i++){
   mesh.attrs[corner_prev_attribute].push_back(ids[(i+3)%4]);
   mesh.attrs[corner_next_attribute].push_back(ids[(i+1)%4]);
  }
 }
 ~Fixture(){free_layer(m.sculpt_layers[0]);for(int i=0;i<4;i++)delete[]mesh.corner_data.grids[i].disps;delete[]mesh.corner_data.grids;}
};
int main(){
 {Fixture f({5,6,7,8});
  assert(remap_after_topology_edit(f.m,f.mesh,nullptr,{}));
  for(int i=0;i<4;i++)assert(f.m.sculpt_layers[0].disps[i].disps==nullptr);
  std::puts("T1 unchanged quad IDs 5/6/7/8, default old IDs: all four real layer payloads discarded");
 }
 {Fixture f({5,6,7,8});int old_ids[]={5,6,7,8};
  assert(remap_after_topology_edit(f.m,f.mesh,nullptr,Span<int>(old_ids,4)));
  for(int i=0;i<4;i++)assert(f.m.sculpt_layers[0].disps[i].disps[0][0]==float(i+5));
  std::puts("T2 explicit stable-ID control: same quad and payloads retained");
 }
 {Fixture f({1,2,3,4});
  assert(remap_after_topology_edit(f.m,f.mesh,nullptr,{}));
  for(int i=0;i<4;i++)assert(f.m.sculpt_layers[0].disps[i].disps[0][0]==float(i+5));
  std::puts("T3 canonical-ID control: default old IDs retain all payloads");
 }
 {Fixture f({5,6,7,8},8);int before_edit[]={1,2,3,4,5,6,7,8};
  assert(remap_after_topology_edit(f.m,f.mesh,nullptr,Span<int>(before_edit,8)));
  assert(f.m.sculpt_layers[0].grids_num==4);
  for(int i=0;i<4;i++)assert(f.m.sculpt_layers[0].disps[i].disps[0][0]==float(i+9));
  assert(remap_after_topology_edit(f.m,f.mesh,nullptr,{}));
  for(int i=0;i<4;i++)assert(f.m.sculpt_layers[0].disps[i].disps==nullptr);
  std::puts("T4 real two-step remap: retain IDs 5-8 from old 1-8, then default unchanged-topology call discards retained detail");
 }
 {MultiresModifierData m;MultiresColorAttribute inactive;
  inactive.grids=MEM_new_array_zeroed<MultiresColorGrid>(4,"inactive paint");inactive.grids_num=4;inactive.level=1;
  inactive.grids[0].colors=new float[4][4]{};inactive.grids[0].colors[0][0]=0.25f;
  m.sculpt_color_attributes.push_back(inactive);
  assert(!editmesh_companion_gate(&m));
  m.sculpt_layers.resize(1);assert(editmesh_companion_gate(&m));
  std::puts("T5 extracted Edit gate: stored inactive Polypaint alone is skipped; adding a layer enables the gate");
  delete[]inactive.grids[0].colors;delete[]inactive.grids;
 }
}
'''

def main():
 text=(SRC/'source/blender/blenkernel/intern/multires_sculpt_layers.cc').read_text()
 sigs=['static void free_layer(','void polypaint_clear(','void faceset_clear(',
       'bool remap_surviving_corners(','static bool initialize_missing_base_grid(',
       'bool remap_after_topology_edit(']
 bodies=[extract(text,s) for s in sigs]
 build=HERE/'build-corner-persistence';build.mkdir(exist_ok=True)
 edit=(SRC/'source/blender/editors/mesh/editmesh_utils.cc').read_text()
 gate=re.search(r'if \((!mmd->sculpt_layers\.is_empty\(\) \|\| mmd->sculpt_color_grids \|\|\s*mmd->sculpt_face_set_grids)\)',edit).group(1)
 gate_fn='bool editmesh_companion_gate(MultiresModifierData*mmd){return '+gate+';}\n'
 cpp=build/'corners.cc';cpp.write_text(ADAPTERS+'\n'.join(bodies)+gate_fn+TESTS)
 flags=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(SRC)]
 subprocess.run(flags+[str(cpp),'-o',str(build/'corners')],check=True)
 result=subprocess.run([str(build/'corners')],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),text=True,capture_output=True)
 if result.returncode:raise RuntimeError(result.stdout+result.stderr)
 (HERE/'corner-persistence-results.txt').write_text(result.stdout)
 manifest={'audited_reference_commit':'01194d089eb5b324b125d0ef6ebaf877dd13f326',
  'scope':'Real remap_after_topology_edit and remap_surviving_corners bodies and identity kernel; minimal Mesh attribute/storage adapters; L0 and ID-property wrappers stubbed; no BMesh or Blender build',
  'functions':{s:hashlib.sha256(b.encode()).hexdigest()for s,b in zip(sigs,bodies)},
  'editmesh_guard_sha256':hashlib.sha256(gate.encode()).hexdigest(),
  'checks':'5 cases: noncanonical/default, explicit-ID, canonical-ID, two-step remap, inactive Polypaint guard',
  'sanitizers':'ASan/UBSan no diagnostics; LeakSanitizer disabled for executor /proc restriction'}
 (HERE/'corner-persistence-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 print(result.stdout,end='')

if __name__=='__main__':main()
