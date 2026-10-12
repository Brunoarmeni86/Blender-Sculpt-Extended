"""Check actual effective-weight and zero-delta routing guards, not native CCG."""
from pathlib import Path
import subprocess
import tempfile
r=Path(__file__).resolve().parents[2]
source=r/'source/blender/makesrna/intern/rna_modifier.cc'
if not source.exists():source=r/'rna_modifier.after.cc'
s=source.read_text()
a=s.index('static float rna_multires_effective_layer_weight');b=s.index('\n}\n',a)+3
helper=s[a:b]
a=s.index('  /* A new zero strength still needs');b=s.index('  if (layer.level >',a)
guard=s[a:b]
cpp='''#include <cassert>
#include <cmath>
#include <limits>
#include <cstdio>
struct Layer{float strength;};
'''+helper+'''bool route(const Layer &layer,float direct_weight_delta,const char *&r_reason){\n'''+guard+'''r_reason="DIRECT";return true;}
int main(){
 const char *reason=nullptr;
 auto check=[&](float old,float now,bool expected_direct){
  const float delta=rna_multires_effective_layer_weight(now)-rna_multires_effective_layer_weight(old);
  assert(route({now},delta,reason));
  assert((reason[0]=='D')==expected_direct);
 };
 check(-3,0,true);check(3,0,true);check(0,.2f,true);check(.2f,0,true);
 check(0,0,false);check(1e-7f,0,false);check(.5f,1e-7f,true);check(1e-7f,.5f,true);
 assert(!route({1},std::numeric_limits<float>::quiet_NaN(),reason));
 std::puts("PASS: nonzero-to-zero, zero-to-nonzero, effective threshold and invalid-weight routing.");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cc';p.write_text(cpp);exe=Path(d)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
