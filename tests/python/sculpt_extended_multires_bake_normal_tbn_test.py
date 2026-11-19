"""Exercise the actual Normal HQ finalize method with portable math services.

Checks RGB encode/decode through the Cycles-style raw interpolated frame,
mirrored handedness, legacy opt-out and neutral fallback. Not a native render test.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile

from sculpt_extended_multires_bake_coverage_test import extract
from sculpt_extended_multires_bake_tbn_audit import MOCKS


TEST = r'''
int main(int argc,char **){
  const bool legacy=argc>1;
  NormalBaker baker;double max_error=0;int cases=0;
  for(int sign:{-1,1})for(float curvature:{.1f,.3f,.6f,1.0f})for(float twist:{0.0f,.2f,.5f}){
    RasterizeTriangle t;
    t.normals[0]={0,0,1};t.normals[1]=math::normalize({curvature,0,1});t.normals[2]=math::normalize({0,curvature,1});
    for(int i=0;i<3;++i){float3 tangent={1,twist*i,0};tangent-=t.normals[i]*math::dot(t.normals[i],tangent);
      tangent=math::normalize(tangent);t.uv_tangents[i]={tangent.x,tangent.y,tangent.z,float(sign)};}
    for(float2 bary: {float2{.2f,.5f},float2{.33f,.33f},float2{.7f,.1f}}){
      const float3x3 render=get_from_tangent_matrix_tbn(t,bary);
      for(float3 target: {float3{.3f,-.2f,1},float3{-.2f,.4f,1},float3{0,0,1}}){
        target=math::normalize(target);const float3 sum=target*16;
        const float3 rgb=baker.finalize_quality_samples_at_pixel(t,bary,sum,16);
        assert(std::isfinite(rgb.x)&&std::isfinite(rgb.y)&&std::isfinite(rgb.z));
        const float3 decoded=(rgb-float3(.5f,.5f,.5f))*2;
        max_error=std::max(max_error,angle(render*decoded,target));
        if(legacy){
          const float3 old_tangent=math::normalize(get_to_tangent_matrix_tbn_hq(t,bary)*math::normalize(sum));
          const float3 old_rgb=old_tangent*.5f+float3(.5f,.5f,.5f);
          assert(std::memcmp(&rgb,&old_rgb,sizeof(rgb))==0);
        }
        ++cases;
      }
    }
  }
  if(legacy)assert(max_error>.1);else assert(max_error<.001);
  // The native invert API returns a zero matrix on a singular frame.
  {RasterizeTriangle t;for(int i=0;i<3;++i){t.normals[i]={0,0,1};t.uv_tangents[i]={0,0,0,1};}
   const auto rgb=baker.finalize_quality_samples_at_pixel(t,{.2f,.3f},{1,1,1},16);
   assert(rgb.x==.5f&&rgb.y==.5f&&rgb.z==1);}
  // No tangents retains the pre-existing identity-matrix behavior.
  {RasterizeTriangle t;t.has_uv_tangents=false;const auto rgb=baker.finalize_quality_samples_at_pixel(t,{.2f,.3f},{0,0,16},16);
   assert(rgb.x==.5f&&rgb.y==.5f&&rgb.z==1);}
  // Zero accumulated directions uses the reference surface normal.
  {RasterizeTriangle t;for(int i=0;i<3;++i){t.normals[i]={0,0,1};t.uv_tangents[i]={1,0,0,1};}
   const auto rgb=baker.finalize_quality_samples_at_pixel(t,{.2f,.3f},{0,0,0},0);
   assert(rgb.x==.5f&&rgb.y==.5f&&rgb.z==1);}
  std::printf("Normal finalize RGB round-trip: %d frames, mode=%s, max angular error %.8f degrees; fallbacks passed.\n",cases,legacy?"previous HQ":"Cycles inverse",max_error);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (pathlib.Path(__file__).resolve().parents[2] / "source/blender/render/intern/multires_bake.cc").read_text()
    mocks = MOCKS.replace("#include <cstdio>", "#include <cstdio>\n#include <cstdlib>\n#include <cstring>")
    mocks = mocks.replace("  float x,y,z;", "  float x,y,z;\n  float3() : x(0),y(0),z(0) {}\n  float3(float a,float b,float c):x(a),y(b),z(c) {}")
    mocks = mocks.replace("assert(std::abs(det)>1e-8f);", "if(det==0)return {{0,0,0},{0,0,0},{0,0,0}};")
    mocks += "float3 operator-(float3 a,float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}\n"
    mocks += "const char *BLI_getenv(const char *name){return std::getenv(name);}\n"
    mocks += "struct BaseBaker {virtual float3 finalize_quality_samples_at_pixel(const RasterizeTriangle &,const float2 &,const float3 &,int)const=0;};\n"
    functions = "\n".join(extract(source, name) for name in (
        "get_from_tangent_matrix_tbn", "get_to_tangent_matrix_tbn", "get_to_tangent_matrix_tbn_hq", "multires_bake_normal_cycles_tbn_enabled"))
    normal_scope = source[source.index("class MultiresNormalsBaker") :]
    method = extract(normal_scope, "finalize_quality_samples_at_pixel")
    cpp = mocks + functions + "struct NormalBaker:BaseBaker {" + method + "};\n" + TEST
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("C++ compiler required")
    with tempfile.TemporaryDirectory() as temp:
        path = pathlib.Path(temp) / "normal.cc"
        exe = pathlib.Path(temp) / "normal"
        path.write_text(cpp)
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([compiler, *flags, str(path), "-o", str(exe)], check=True)
        for mode in ("1", "0"):
            env = dict(os.environ, BLENDER_MULTIRES_BAKE_NORMAL_CYCLES_TBN=mode)
            subprocess.run([str(exe)] + (["legacy"] if mode == "0" else []), env=env, check=True)


if __name__ == "__main__":
    main()
