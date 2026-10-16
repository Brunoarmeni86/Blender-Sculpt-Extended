#!/usr/bin/env python3
"""Extract audited functions unchanged and run against minimal storage/API adapters.

This is not a Blender integration test. Run from this directory with downloaded
source/ at audited commit 01194d089eb5b324b125d0ef6ebaf877dd13f326.
"""
from pathlib import Path
import subprocess
import hashlib
import json
import os
import sys

ROOT = Path(__file__).resolve().parent
SOURCE_ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT
SOURCE = SOURCE_ROOT / 'source/blender/blenkernel/intern/multires_sculpt_layers.cc'

def extract(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    pos = brace + 1
    while depth:
        if text[pos] == '{': depth += 1
        elif text[pos] == '}': depth -= 1
        pos += 1
    return text[start:pos]

PREAMBLE = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <limits>
#include <new>
#include <vector>
#include <cassert>
#include "source/blender/blenkernel/intern/multires_layer_base_math.hh"
struct float3 { float x, y, z; explicit float3(float v=0):x(v),y(v),z(v){} };
template<class T> struct Array : std::vector<T> { using std::vector<T>::vector; };
template<class T> T* MEM_new_array_zeroed(size_t n, const char*) { return (T*)calloc(n,sizeof(T)); }
template<class T> T* MEM_new_array_uninitialized(size_t n, const char*) { return (T*)malloc(n*sizeof(T)); }
template<class T> void MEM_delete(T* p) { free(p); }
#define MEM_SAFE_DELETE(p) do { free(p); (p)=nullptr; } while(0)
template<size_t N> void BLI_strncpy(char (&out)[N], const char* in, size_t n) { std::snprintf(out,n,"%s",in); }
struct MDisps { float (*disps)[3]=nullptr; unsigned int* hidden=nullptr; int totdisp=4, level=1; };
struct MultiresSculptLayer {
  MultiresSculptLayer *next=nullptr,*prev=nullptr;
  char name[64]="Layer"; float strength=1;
  int flag=1,level=1,grids_num=1;
  MDisps* disps=nullptr; float (*base_deltas)[3]=nullptr; int base_verts_num=0;
  int group_id=0,group_flag=0; char group_name[64]="";
};
struct List {
  void *first=nullptr,*last=nullptr;
  struct It { MultiresSculptLayer* p;
    MultiresSculptLayer& operator*()const{return *p;}
    It& operator++(){p=p->next;return *this;}
    bool operator!=(const It& o)const{return p!=o.p;}
  };
  It begin()const{return {static_cast<MultiresSculptLayer*>(first)};}
  It end()const{return {nullptr};}
  bool is_empty()const{return !first;}
  int count()const{int n=0;for(auto &v:*this){(void)v;++n;}return n;}
};
struct MultiresFaceSetGrid { int* ids=nullptr; int totid=0,level=1,uniform_id=1; };
struct MultiresModifierData {
  List sculpt_layers; int sculpt_layer_active=0,sculpt_layer_recording=0;
  MultiresFaceSetGrid* sculpt_face_set_grids=nullptr;
  int sculpt_face_set_grids_num=0,sculpt_face_set_level=0;
};
struct Bytes {
  std::vector<unsigned char> v;
  const Bytes& as_span()const{return *this;}
  size_t size_in_bytes()const{return v.size();}
};
struct Bits { int n=0;int size()const{return n;}const Bits& all_bits()const{return *this;} };
struct Node {
  Bytes position,orig_position,normal,col,mask,loop_col,vert_indices,corner_indices;
  Bits vert_hidden,face_hidden;
  Bytes grids; Bits grid_hidden;
  Bytes face_sets,face_indices,grid_face_sets; Bits grid_color_present;
};
constexpr int MULTIRES_SCULPT_LAYER_VISIBLE=1;
constexpr int MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE=2;
constexpr int MULTIRES_SCULPT_LAYER_GROUP_HIDDEN=1;
void* BLI_findlink(const List* list,int i){auto *p=(MultiresSculptLayer*)list->first;while(p&&i-->0)p=p->next;return p;}
int BLI_findindex(const List* list,const void* find){int i=0;for(auto &l:*list){if(&l==find)return i;++i;}return -1;}
void BLI_remlink(List* list,MultiresSculptLayer* p){
  if(p->prev)p->prev->next=p->next;else list->first=p->next;
  if(p->next)p->next->prev=p->prev;else list->last=p->prev;
}
namespace blender::bke::multires_layers {
namespace subdiv { int grid_size_from_level(int level){return (1<<(level-1))+1;} }
void weight_hot_delta_cache_invalidate(const MultiresSculptLayer* =nullptr){}
bool merge_down(MultiresModifierData&);
'''

TESTS = r'''
} // namespace blender::bke::multires_layers
using namespace blender::bke::multires_layers;
static MultiresSculptLayer* add_layer(MultiresModifierData& m, float value, bool dense=true, int group=0){
  auto *p=(MultiresSculptLayer*)malloc(sizeof(MultiresSculptLayer));new(p)MultiresSculptLayer{};
  p->disps=(MDisps*)malloc(sizeof(MDisps));new(p->disps)MDisps{};
  if(dense){p->disps[0].disps=MEM_new_array_zeroed<float[3]>(4,"test");p->disps[0].disps[0][0]=value;}
  p->group_id=group;p->prev=(MultiresSculptLayer*)m.sculpt_layers.last;
  if(p->prev)p->prev->next=p;else m.sculpt_layers.first=p;
  m.sculpt_layers.last=p;return p;
}
static void cleanup(MultiresModifierData& m){while(m.sculpt_layers.first){auto *p=(MultiresSculptLayer*)m.sculpt_layers.first;BLI_remlink(&m.sculpt_layers,p);free_layer(*p);free(p);}faceset_clear(m);}
int main(){
  { MultiresModifierData m;add_layer(m,2);auto *dst=add_layer(m,3);auto *other=add_layer(m,9);
    bool ok=merge_down(m);assert(ok&&dst->disps[0].disps[0][0]==5);
    assert(active(m)==other);printf("R1 merge_down: sum=5, active=unrelated third layer (expected destination)\n");cleanup(m);
  }
  { MultiresModifierData m;add_layer(m,2,true,7);add_layer(m,3,true,7);add_layer(m,9,true,7);
    bool ok=group_merge(m,7);assert(ok&&m.sculpt_layers.count()==2);
    auto *first=(MultiresSculptLayer*)m.sculpt_layers.first;auto *last=(MultiresSculptLayer*)m.sculpt_layers.last;
    assert(first->group_id==7&&last->group_id==0);
    printf("R2 group_merge: success=1, remaining=2 (expected 1), residual group member=1\n");cleanup(m);
  }
  { MultiresModifierData m;add_layer(m,2,true,7);add_layer(m,3,true,7);add_layer(m,9,true,0);
    bool ok=group_merge(m,7);assert(!ok&&m.sculpt_layers.count()==2);
    printf("R3 group_merge: failure returned after two source layers already merged\n");cleanup(m);
  }
  { MultiresModifierData m;auto *dst=add_layer(m,0,false);add_layer(m,7);
    flatten(m);assert(m.sculpt_layers.count()==1&&dst->disps[0].disps==nullptr);
    printf("R4 flatten sparse: source detail 7 deleted, destination remains implicit zero\n");cleanup(m);
  }
  { MultiresModifierData m;auto *dst=add_layer(m,2);add_layer(m,7);
    flatten(m);assert(m.sculpt_layers.count()==1&&dst->disps[0].disps[0][0]==9);
    printf("C1 flatten dense control: expected sum 9 preserved\n");cleanup(m);
  }
  { MultiresModifierData m;m.sculpt_face_set_grids_num=2;m.sculpt_face_set_level=1;
    m.sculpt_face_set_grids=(MultiresFaceSetGrid*)calloc(2,sizeof(MultiresFaceSetGrid));
    for(int i=0;i<2;i++){auto &g=m.sculpt_face_set_grids[i];g.ids=MEM_new_array_zeroed<int>(4,"test");g.level=1;g.totid=4;}
    m.sculpt_face_set_grids[1].totid=3; // Explicit malformed-input validation scenario.
    bool ok=faceset_resample_up(m,2);
    assert(!ok&&m.sculpt_face_set_level==1&&m.sculpt_face_set_grids[0].level==2);
    printf("R5 faceset resample: failure leaves master level 1 but first grid level 2\n");cleanup(m);
  }
  { MultiresModifierData m;auto *dst=add_layer(m,2);auto *src=add_layer(m,7);
    dst->disps[0].disps[0][0]=std::numeric_limits<float>::max();src->strength=2;
    // Make second operand overflow too; finite source payloads and strengths.
    src->disps[0].disps[0][0]=std::numeric_limits<float>::max();
    bool ok=merge_down(m);assert(ok&&!std::isfinite(active(m)->disps[0].disps[0][0]));
    printf("R6 merge finite overflow: success=1, merged high-resolution sample=Inf\n");cleanup(m);
  }
  { Node n;auto before=node_size_in_bytes(n);n.grid_face_sets.v.resize(4*1024*1024);
    auto after=node_size_in_bytes(n);assert(before==after);
    printf("R7 Undo accounting: allocated 4 MiB Face Set master snapshot, reported size delta=0\n");
  }
}
'''

def main():
    text = SOURCE.read_text()
    signatures = ['bool base_deltas_ensure(', 'static void free_layer(',
                  'MultiresSculptLayer *active(MultiresModifierData &mmd)',
                  'float (*grid_ensure(', 'bool merge_down(', 'void flatten(',
                  'bool group_merge(', 'void faceset_clear(', 'bool faceset_resample_up(']
    functions = [extract(text, s) for s in signatures]
    undo_text = (SOURCE_ROOT/'source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc').read_text()
    signatures.append('static size_t node_size_in_bytes(')
    functions.append(extract(undo_text,signatures[-1]))
    build = ROOT / 'build'; build.mkdir(exist_ok=True)
    (build/'functions.cc').write_text(PREAMBLE+'\n\n'.join(functions)+'\n'+TESTS)
    cmd = ['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(SOURCE_ROOT),
           str(build/'functions.cc'),'-o',str(build/'verify')]
    subprocess.run(cmd, check=True)
    # LeakSanitizer cannot enumerate /proc tasks in the executor. Keep ASan/UBSan.
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    result = subprocess.run([str(build/'verify')], check=True, capture_output=True,text=True,env=env)
    (ROOT/'verification-results.txt').write_text(result.stdout)
    print(result.stdout,end='')
    manifest = {'commit':'01194d089eb5b324b125d0ef6ebaf877dd13f326',
                'scope':'Extracted production function bodies; simulated storage/API adapters; not full Blender',
                'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
                'functions':dict(zip(signatures,[hashlib.sha256(s.encode()).hexdigest() for s in functions])),
                'compiler_command':cmd,'ASAN_OPTIONS':'detect_leaks=0 (executor /proc restriction)',
                'result':'8 checks passed; expected defects reproduced'}
    (ROOT/'verification-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__ == '__main__': main()
