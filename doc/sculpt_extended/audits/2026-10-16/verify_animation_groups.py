#!/usr/bin/env python3
"""Third audit: original RNA/animation and group bodies with API adapters.
No complete Blender, dependency graph, playback or .blend IO is simulated.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
from verify_findings import extract, PREAMBLE, TESTS

HERE=Path(__file__).resolve().parent
SRC=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else HERE

ANIM_ADAPTERS=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
struct ID {ID* orig_id=nullptr;bool evaluated=false;};
struct ModifierData {int type=1;int recording=1;};
struct MultiresModifierData {ModifierData modifier;};
#define sculpt_layer_recording modifier.recording
struct MultiresSculptLayer {float strength=1;int flag=1;};
struct Object {ID id;int mode=1;std::vector<ModifierData>modifiers{{}};MultiresSculptLayer* layer=nullptr;};
struct PointerRNA {ID*owner_id=nullptr;void*data=nullptr;void*type=nullptr;};
struct NlaStrip {NlaStrip*orig_strip=nullptr;};
void* RNA_NlaStrip=(void*)1;
struct PropertyRNA {int type=2;};
struct PathResolvedRNA {PropertyRNA*prop=nullptr;PointerRNA ptr;int prop_index=-1;};
template<class T> T id_cast(ID*p){return reinterpret_cast<T>(p);}
constexpr int OB_MODE_SCULPT=1,eModifierType_Multires=1,MULTIRES_SCULPT_LAYER_VISIBLE=1;
constexpr int ID_RECALC_GEOMETRY=1,ID_RECALC_SYNC_TO_EVAL=2;
int commit_calls=0,rebuild_calls=0,refresh_calls=0,tag_calls=0;
bool commit_result=true;
bool DEG_is_evaluated_id(ID*id){return id->evaluated;}
void DEG_id_tag_update(ID*,int){++tag_calls;}
bool multires_force_sculpt_rebuild(Object*){++rebuild_calls;return false;}
namespace bke::multires_layers {bool commit(Object&){++commit_calls;return commit_result;}}
bool rna_multires_visible_trace_enabled(){return false;}
double BLI_time_now_seconds(){return 0;}
bool rna_multires_visible_fast_refresh(Object&,const MultiresSculptLayer&,float,int&,int&,int&,double&,double&,const char*&){++refresh_calls;return true;}
enum {PROP_BOOLEAN,PROP_INT,PROP_FLOAT,PROP_ENUM};
#define BLI_assert assert
#define ANIMSYS_FLOAT_AS_BOOL(x) ((x)!=0)
int RNA_property_type(PropertyRNA*p){return p->type;}
bool RNA_property_animateable(PointerRNA*,PropertyRNA*){return true;}
void RNA_property_boolean_set_index(PointerRNA*,PropertyRNA*,int,int){}
void RNA_property_boolean_set(PointerRNA*,PropertyRNA*,int){}
void RNA_property_int_clamp(PointerRNA*,PropertyRNA*,int*){}
void RNA_property_int_set_index(PointerRNA*,PropertyRNA*,int,int){}
void RNA_property_int_set(PointerRNA*,PropertyRNA*,int){}
void RNA_property_float_clamp(PointerRNA*,PropertyRNA*,float*){}
void RNA_property_enum_set(PointerRNA*,PropertyRNA*,int){}
static void rna_MultiresSculptLayer_strength_set(PointerRNA*,float);
void RNA_property_float_set(PointerRNA*p,PropertyRNA*,float v){rna_MultiresSculptLayer_strength_set(p,v);}
void RNA_property_float_set_index(PointerRNA*p,PropertyRNA*q,int,float v){RNA_property_float_set(p,q,v);}
bool BKE_animsys_read_from_rna_path(PathResolvedRNA*r,float*out){*out=((MultiresSculptLayer*)r->ptr.data)->strength;return true;}
bool BKE_animsys_write_to_rna_path(PathResolvedRNA*,float,bool force_write=false);
bool BKE_animsys_rna_path_resolve(PointerRNA*root,const char*,int,PathResolvedRNA*out){
  static PropertyRNA strength;auto*ob=reinterpret_cast<Object*>(root->owner_id);
  out->ptr={&ob->id,ob->layer,nullptr};out->prop=&strength;out->prop_index=-1;return ob->layer!=nullptr;
}
'''

ANIM_TESTS=r'''
static void reset_counts(){commit_calls=rebuild_calls=refresh_calls=tag_calls=0;commit_result=true;}
int main(){
  MultiresSculptLayer original_layer,evaluated_layer;
  Object original,evaluated;original.layer=&original_layer;evaluated.layer=&evaluated_layer;
  evaluated.id.evaluated=true;evaluated.id.orig_id=&original.id;
  PointerRNA eval_prop{&evaluated.id,&evaluated_layer,nullptr},eval_root{&evaluated.id,&evaluated,nullptr};
  reset_counts();rna_MultiresSculptLayer_strength_set(&eval_prop,0.5f);
  assert(evaluated_layer.strength==0.5f&&commit_calls==0&&evaluated.modifiers[0].recording==1);
  std::puts("A1 evaluated-ID control: scalar updated, no Commit, REC preserved");
  reset_counts();animsys_write_orig_anim_rna(&eval_root,"modifiers[\"Multires\"].sculpt_layers[\"Layer\"].strength",-1,0.5f);
  assert(original_layer.strength==0.5f&&commit_calls==1&&refresh_calls==1&&original.modifiers[0].recording==0);
  std::puts("A2 animation copy-back to original Sculpt ID: Commit=1, refresh=1, REC 1->0");
  reset_counts();animsys_write_orig_anim_rna(&eval_root,"strength",-1,0.5f);
  assert(commit_calls==0&&refresh_calls==0);
  std::puts("A3 unchanged-value control: copy-back skips setter side effects");
  original.mode=0;original.modifiers[0].recording=1;
  reset_counts();animsys_write_orig_anim_rna(&eval_root,"strength",-1,0.25f);
  assert(original_layer.strength==0.25f&&commit_calls==0&&original.modifiers[0].recording==1);
  std::puts("A4 original Object Mode control: scalar updated without Commit or REC change");
  original.mode=OB_MODE_SCULPT;original_layer.strength=1;original.modifiers[0].recording=1;
  reset_counts();commit_result=false;
  animsys_write_orig_anim_rna(&eval_root,"strength",-1,0.125f);
  assert(commit_calls==1&&rebuild_calls==1&&original_layer.strength==1&&original.modifiers[0].recording==1);
  std::puts("A5 simulated Commit refusal: original copy-back leaves old scalar, rebuild requested");
}
'''

GROUP_TESTS=r'''
int main(){
  {MultiresModifierData m;add_layer(m,1,true,7);auto*b=add_layer(m,1,true,7);add_layer(m,1,true,7);
   m.sculpt_layer_active=1;assert(group_remove_active(m)&&b->group_id==0);
   assert(!group_merge(m,7)&&m.sculpt_layers.count()==3);
   std::puts("G1 remove middle member: group IDs 7/0/7; Merge Group rejected as non-contiguous");cleanup(m);}
  {MultiresModifierData m;add_layer(m,1,true,7);add_layer(m,1,true,7);add_layer(m,1,true,7);
   assert(group_remove_active(m));assert(group_merge(m,7)&&m.sculpt_layers.count()==2);
   std::puts("G2 endpoint-removal control: remaining contiguous members merge successfully");cleanup(m);}
  {MultiresModifierData m;auto*a=add_layer(m,1,true,1);auto*b=add_layer(m,1,true,2);
   assert(group_toggle_visibility(m,2));assert(!(b->flag&MULTIRES_SCULPT_LAYER_VISIBLE));
   assert(b->flag&MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE);
   assert(group_with_below(m));assert(a->group_id==b->group_id&&b->group_flag==0);
   assert(!(b->flag&MULTIRES_SCULPT_LAYER_VISIBLE));
   assert(group_toggle_visibility(m,1)&&group_toggle_visibility(m,1));
   assert(!(b->flag&MULTIRES_SCULPT_LAYER_VISIBLE)&&!(b->flag&MULTIRES_SCULPT_LAYER_GROUP_SAVED_VISIBLE));
   std::puts("G3 merge hidden group into visible group: member stays hidden; next hide/show loses saved visibility");cleanup(m);}
  {MultiresModifierData m;add_layer(m,1,true,1);auto*b=add_layer(m,1,true,2);
   assert(group_toggle_visibility(m,2));m.sculpt_layer_active=1;
   assert(group_add_active(m,1));assert(b->group_id==1&&(b->flag&MULTIRES_SCULPT_LAYER_VISIBLE));
   std::puts("G4 Add Active to Group control: same hidden->visible transition restores member visibility");cleanup(m);}
}
'''

def main():
    rna=(SRC/'source/blender/makesrna/intern/rna_modifier.cc').read_text()
    anim=(SRC/'source/blender/blenkernel/intern/anim_sys.cc').read_text()
    layers=(SRC/'source/blender/blenkernel/intern/multires_sculpt_layers.cc').read_text()
    specs=[('rna', 'static void rna_MultiresSculptLayer_strength_set('),
           ('anim','bool BKE_animsys_write_to_rna_path('),
           ('anim','static bool animsys_construct_orig_pointer_rna('),
           ('anim','static void animsys_write_orig_anim_rna(')]
    texts={'rna':rna,'anim':anim,'layers':layers}
    bodies=[extract(texts[k],s) for k,s in specs]
    group_sigs=['bool base_deltas_ensure(','static void free_layer(',
      'MultiresSculptLayer *active(MultiresModifierData &mmd)','float (*grid_ensure(',
      'bool merge_down(','bool group_merge(','void faceset_clear(',
      'static int group_next_id(','static void group_assign(',
      'bool group_with_below(','bool group_add_active(',
      'bool group_remove_active(','bool group_toggle_visibility(']
    group_bodies=[extract(layers,s) for s in group_sigs]
    helper=TESTS[:TESTS.index('int main()')]
    group_extra=r'''
#include <string>
#define BLI_snprintf std::snprintf
void BLI_insertlinkafter(List*l,MultiresSculptLayer*before,MultiresSculptLayer*p){
 p->prev=before;p->next=before->next;if(p->next)p->next->prev=p;else l->last=p;before->next=p;
}
'''
    build=HERE/'build-animation-groups';build.mkdir(exist_ok=True)
    (build/'animation.cc').write_text(ANIM_ADAPTERS+'\n'.join(bodies)+ANIM_TESTS)
    # Insert list insertion adapter before entering the production namespace.
    preamble=PREAMBLE.replace('namespace blender::bke::multires_layers {',group_extra+'\nnamespace blender::bke::multires_layers {')
    (build/'groups.cc').write_text(preamble+'\n'.join(group_bodies)+helper+GROUP_TESTS)
    flags=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(SRC)]
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    results=[]
    for target in ['animation','groups']:
        subprocess.run(flags+[str(build/f'{target}.cc'),'-o',str(build/target)],check=True)
        r=subprocess.run([str(build/target)],env=env,text=True,capture_output=True)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        results.append(r.stdout)
    output=''.join(results)
    (HERE/'animation-groups-results.txt').write_text(output)
    manifest={'audited_reference_commit':'01194d089eb5b324b125d0ef6ebaf877dd13f326',
      'scope':'Original setters/animation dispatch/group bodies; RNA path resolution, Commit and refresh are adapters; no Blender playback',
      'functions':{s:hashlib.sha256(b.encode()).hexdigest() for (_,s),b in zip(specs,bodies)},
      'group_functions':{s:hashlib.sha256(b.encode()).hexdigest() for s,b in zip(group_sigs,group_bodies)},
      'checks':'9 observations and controls: A1-A5, G1-G4',
      'sanitizers':'ASan/UBSan; no diagnostics; LeakSanitizer disabled due to executor /proc restriction'}
    (HERE/'animation-groups-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(output,end='')

if __name__=='__main__':main()
