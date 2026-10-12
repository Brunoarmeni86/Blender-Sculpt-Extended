"""Compile real ID lookup/counter helpers; simulate Delete/Fill and old-layout restore.

No Blender BMesh, CCG geometry, or native UndoStack is executed here.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]


def main():
    source=(ROOT/"source/blender/blenkernel/intern/multires_sculpt_layers.cc").read_text()
    start=source.index("static constexpr char corner_counter_property[]")
    counter=source[start:source.index("void ensure_corner_ids(",start)]
    header=ROOT/"source/blender/blenkernel/intern/multires_layer_corner_identity.hh"
    cpp=r'''
#include <algorithm>
#include <cassert>
#include <climits>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "HEADER_PATH"
using blender::bke::multires_layers::detail::corner_identity_lookup;
constexpr int IDP_INT=1;
struct IDProperty {int type=IDP_INT;int value=0;std::string name;
                   std::unordered_map<std::string,std::unique_ptr<IDProperty>> children;};
struct ID {std::unique_ptr<IDProperty> system;};
struct Mesh {ID id;};
IDProperty *IDP_ID_system_properties_get(ID *id){return id->system.get();}
IDProperty *IDP_ID_system_properties_ensure(ID *id){
 if(!id->system){id->system=std::make_unique<IDProperty>();}
 return id->system.get();
}
IDProperty *IDP_GetPropertyFromGroup_null(IDProperty *group,const char *key){
 if(!group){return nullptr;}
 auto found=group->children.find(key);
 return found==group->children.end()?nullptr:found->second.get();
}
IDProperty *IDP_NewInt(int value,const char *name){
 auto *p=new IDProperty;p->value=value;p->name=name;return p;
}
bool IDP_AddToGroup(IDProperty *group,IDProperty *property){
 group->children[property->name]=std::unique_ptr<IDProperty>(property);return true;
}
#define IDP_int_get(prop) ((prop)->value)
#define IDP_int_set(prop,val) ((prop)->value=(val))
COUNTER_BODY
struct Result {std::vector<int> map,counts;int max_id=0;};
bool lookup(const std::vector<int> &old_ids,const std::vector<int> &new_ids,Result &r){
 r.map.resize(new_ids.size());r.counts.resize(old_ids.size());
 return corner_identity_lookup(old_ids.data(),int(old_ids.size()),new_ids.data(),
   int(new_ids.size()),r.map.data(),r.counts.data(),r.max_id);
}
int main(){
 Result r;
 assert(lookup({11,19,7},{7,11,19},r));
 assert((r.map==std::vector<int>{2,0,1}));
 assert((r.counts==std::vector<int>{1,1,1}));
 assert(r.max_id==19);
 assert(lookup({11,19,7},{7,7,0,99},r));
 assert((r.map==std::vector<int>{2,2,-1,-1}));assert(r.counts[2]==2);
 assert(!lookup({5,5},{5},r));
 assert(!lookup({0},{0},r));
 assert(!lookup({-1},{-1},r));
 assert(lookup({}, {},r));
 assert(lookup({}, {1,2},r));assert((r.map==std::vector<int>{-1,-1}));
 assert(lookup({INT_MAX},{INT_MAX},r));assert(r.max_id==INT_MAX);

 // Original six quad faces. Delete face 1; append a zero-delta Fill face.
 std::vector<int> original(24);std::iota(original.begin(),original.end(),1);
 std::vector<int> live_ids(original.begin(),original.begin()+4);
 live_ids.insert(live_ids.end(),original.begin()+8,original.end());
 live_ids.insert(live_ids.end(),{25,26,27,28});
 std::vector<int> payload(live_ids.begin(),live_ids.begin()+20);
 payload.insert(payload.end(),4,0);
 assert(live_ids.size()==original.size());
 int old_wrong=0;for(int i=0;i<24;i++)old_wrong+=payload[i]!=original[i];
 assert(old_wrong==20); // Old identity test accepted all 24 restored indices.
 assert(lookup(live_ids,original,r));
 for(int i=0;i<24;i++){
   const int restored=r.map[i]<0?0:payload[r.map[i]];
   if(i>=4&&i<8){assert(restored==0);} // Deleted payload needs a real undo checkpoint.
   else{assert(restored==original[i]);} // Every surviving payload stays on its own corner.
 }

 Mesh mesh;assert(corner_id_counter(mesh)==0);
 corner_id_counter_set(mesh,28);assert(corner_id_counter(mesh)==28);
 corner_id_counter_set(mesh,20);assert(corner_id_counter(mesh)==28);
 corner_id_counter_set(mesh,32);assert(corner_id_counter(mesh)==32);
 // Geometry deletion/BMesh replacement does not reset the ID-owned high-water mark.
 live_ids.clear();assert(corner_id_counter(mesh)==32);
 IDP_GetPropertyFromGroup_null(mesh.id.system.get(),corner_counter_property)->type=2;
 assert(corner_id_counter(mesh)==-1);

 std::mt19937 random(19);
 for(int n=1;n<=1000;n++){
   std::vector<int> old_ids(n);std::iota(old_ids.begin(),old_ids.end(),1001);
   std::shuffle(old_ids.begin(),old_ids.end(),random);
   std::vector<int> new_ids=old_ids;
   std::shuffle(new_ids.begin(),new_ids.end(),random);
   assert(lookup(old_ids,new_ids,r));
   for(int i=0;i<n;i++){assert(old_ids[r.map[i]]==new_ids[i]);assert(r.counts[i]==1);}
 }
}
'''.replace("HEADER_PATH",str(header)).replace("COUNTER_BODY",counter)
    with tempfile.TemporaryDirectory() as temp:
        src=Path(temp)/"identity.cc";exe=Path(temp)/"identity"
        src.write_text(cpp)
        subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",
                        "-fsanitize=address,undefined",str(src),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print("PASS: ID edge cases, same-count Delete/Fill restore, persistent counter, 1000 shuffled layouts; ASan/UBSan")


if __name__=="__main__":main()
