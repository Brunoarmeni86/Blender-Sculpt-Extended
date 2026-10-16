#!/usr/bin/env python3
"""Standalone math comparison; no Blender runtime or performance benchmark."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parent
HEADER = ROOT / 'source/blender/blenkernel/intern/polypaint_cpu_math.hh'
BUILD = ROOT / 'build-optimizations'
BUILD.mkdir(exist_ok=True)
CODE = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "source/blender/blenkernel/intern/polypaint_cpu_math.hh"
using namespace blender::bke::multires_polypaint_cpu;
// Experimental variant only: preserves loop order and skips zero-weight stores.
bool candidate(float (*colors)[4], int ms, int ps, int px, int py, const float d[4]) {
  if (!colors || !d || !valid_side(ms) || !valid_side(ps) || ms < ps ||
      px < 0 || py < 0 || px >= ps || py >= ps) return false;
  for(int c=0;c<4;c++) if(!std::isfinite(d[c])) return false;
  const float scale=float(ms-1)/float(ps-1), cx=float(px)*scale, cy=float(py)*scale;
  int xa=std::max(0,int(std::floor(cx-scale))), xb=std::min(ms-1,int(std::ceil(cx+scale)));
  int ya=std::max(0,int(std::floor(cy-scale))), yb=std::min(ms-1,int(std::ceil(cy+scale)));
  for(int y=ya;y<=yb;y++) {
    float wy=std::max(0.0f,1.0f-std::abs(float(y)-cy)/scale);
    for(int x=xa;x<=xb;x++) {
      float wx=std::max(0.0f,1.0f-std::abs(float(x)-cx)/scale), w=wx*wy;
      if(w==0.0f) continue;
      for(int c=0;c<4;c++) colors[y*ms+x][c]+=w*d[c];
    }
  }
  return true;
}
int main() {
  std::mt19937 rng(20261016); int cases=0;
  for(int ms : {2,3,5,9,17,33,65}) for(int ps : {2,3,5,9,17,33,65}) {
    if(ps>ms) continue;
    std::vector<float> a(ms*ms*4), b;
    for(float &v:a) v=0.25f+float(rng()%1024)/2048.0f;
    b=a;
    for(int trial=0;trial<100;trial++) {
      int x=rng()%ps, y=rng()%ps;
      float delta[4]; for(float &v:delta) v=(int(rng()%129)-64)/4096.0f;
      bool xok=add_coarse_delta(reinterpret_cast<float(*)[4]>(a.data()),ms,ps,x,y,delta);
      bool yok=candidate(reinterpret_cast<float(*)[4]>(b.data()),ms,ps,x,y,delta);
      assert(xok && yok && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0);
      cases++;
    }
  }
  std::printf("P1 finite positive color buffers: %d sequential operations bitwise equal\n",cases);
  for(int ratio : {1,2,4,8}) {
    int visited=(2*ratio+1)*(2*ratio+1), nonzero=(2*ratio-1)*(2*ratio-1);
    std::printf("P2 interior ratio=%d: visited=%d nonzero=%d zero_weight=%d\n",
                ratio,visited,nonzero,visited-nonzero);
  }
  float a[9][4], b[9][4];
  for(auto &p:a) for(float &v:p) v=-0.0f;
  std::memcpy(b,a,sizeof(a)); float delta[4]={1,1,1,1};
  assert(add_coarse_delta(a,3,3,1,1,delta) && candidate(b,3,3,1,1,delta));
  assert(!std::signbit(a[0][0]) && std::signbit(b[0][0]));
  std::printf("P3 signed-zero counterexample: original neighbor=+0 candidate=-0; not universally bitwise equivalent\n");
  float bad[4]={NAN,0,0,0};
  assert(!add_coarse_delta(a,3,3,1,1,bad) && !candidate(b,3,3,1,1,bad));
  std::printf("P4 nonfinite delta: both reject\n");
}
'''
(BUILD / 'compare.cc').write_text(CODE)
cmd = ['g++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
       '-fno-omit-frame-pointer', '-I', str(ROOT), str(BUILD / 'compare.cc'),
       '-o', str(BUILD / 'compare')]
subprocess.run(cmd, check=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
result = subprocess.run([str(BUILD / 'compare')], check=True, capture_output=True, text=True, env=env)
assert not result.stderr, result.stderr
(ROOT / 'optimization-results.txt').write_text(result.stdout)
manifest = {
    'checkpoint': '01194d089eb5b324b125d0ef6ebaf877dd13f326',
    'scope': 'Actual math header versus experimental skip-zero variant; no native Blender test, no timings',
    'source': str(HEADER.relative_to(ROOT)),
    'source_git_blob': hashlib.sha1(b'blob '+str(len(HEADER.read_bytes())).encode()+b'\0'+HEADER.read_bytes()).hexdigest(),
    'compile': cmd, 'ASAN_OPTIONS': 'detect_leaks=0', 'stderr': result.stderr,
}
(ROOT / 'optimization-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(result.stdout, end='')
