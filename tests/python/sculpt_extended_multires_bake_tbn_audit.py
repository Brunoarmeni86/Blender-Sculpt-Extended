"""Numerical audit of the current HQ TBN versus the Cycles normal-map basis.

Extracts the actual three baker matrix helpers, with standalone vector/matrix
math replacing Blender services. This reports a reproduced basis mismatch;
it is not a render integration test and makes no production code changes.
"""
import pathlib
import shutil
import subprocess
import tempfile

from sculpt_extended_multires_bake_coverage_test import extract

SOURCE = pathlib.Path(__file__).resolve().parents[2] / "source/blender/render/intern/multires_bake.cc"

MOCKS = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
struct float2 {float x,y;};
struct float3 {
  float x,y,z;
  float3 xyz()const{return *this;}
  float3 &operator-=(float3 b){x-=b.x;y-=b.y;z-=b.z;return *this;}
};
float3 operator+(float3 a,float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
float3 operator*(float3 a,float b){return {a.x*b,a.y*b,a.z*b};}
float3 operator*(float a,float3 b){return b*a;}
float3 operator/(float3 a,float b){return a*(1/b);}
struct float4{float x,y,z,w;float3 xyz()const{return {x,y,z};}};
struct float3x3 {
  float3 x,y,z;
  static float3x3 identity(){return {{1,0,0},{0,1,0},{0,0,1}};}
};
float3 operator*(float3x3 m,float3 v){return m.x*v.x+m.y*v.y+m.z*v.z;}
namespace math {
float dot(float3 a,float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float3 cross(float3 a,float3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float3 normalize(float3 a){return a/std::sqrt(dot(a,a));}
float3x3 invert(float3x3 m){
  const float det=dot(m.x,cross(m.y,m.z));assert(std::abs(det)>1e-8f);
  const float3 a=cross(m.y,m.z)/det,b=cross(m.z,m.x)/det,c=cross(m.x,m.y)/det;
  return {{a.x,b.x,c.x},{a.y,b.y,c.y},{a.z,b.z,c.z}};
}}
struct RasterizeTriangle {
  bool has_uv_tangents=true;float3 normals[3];float4 uv_tangents[3];
  float3 get_normal(float2 b)const{return math::normalize(normals[0]*b.x+normals[1]*b.y+normals[2]*(1-b.x-b.y));}
};
double angle(float3 a,float3 b){
  a=math::normalize(a);b=math::normalize(b);const float3 c=math::cross(a,b);
  return std::atan2(std::sqrt(double(math::dot(c,c))),double(math::dot(a,b)))*180/3.141592653589793;
}
'''

TEST = r'''
int main(){
  double max_raw=0,max_hq=0;int cases=0;
  for(int sign:{-1,1})for(float curvature:{.1f,.3f,.6f,1.0f})for(float twist:{0.0f,.2f,.5f}){
    RasterizeTriangle t;
    t.normals[0]={0,0,1};t.normals[1]=math::normalize({curvature,0,1});t.normals[2]=math::normalize({0,curvature,1});
    for(int i=0;i<3;++i){float3 tangent={1,twist*i,0};tangent-=t.normals[i]*math::dot(t.normals[i],tangent);
      tangent=math::normalize(tangent);t.uv_tangents[i]={tangent.x,tangent.y,tangent.z,float(sign)};}
    for(float2 bary: {float2{.2f,.5f},float2{.33f,.33f},float2{.7f,.1f}}){
      const float3x3 render=get_from_tangent_matrix_tbn(t,bary);
      for(float3 target: {float3{.3f,-.2f,1},float3{-.2f,.4f,1},float3{0,0,1}}){
        target=math::normalize(target);
        const float3 raw=get_to_tangent_matrix_tbn(t,bary)*target;
        const float3 hq=get_to_tangent_matrix_tbn_hq(t,bary)*target;
        max_raw=std::max(max_raw,angle(render*raw,target));max_hq=std::max(max_hq,angle(render*hq,target));++cases;
      }
    }
  }
  // The legacy matrix is an inverse of this renderer basis; HQ changes that basis.
  assert(max_raw<.001);assert(max_hq>.1);
  std::printf("TBN audit: %d synthetic frames, raw inverse max %.8f degrees; current orthonormal HQ max %.6f degrees. Native render validation required.\n",cases,max_raw,max_hq);
}
'''


def main():
    source = SOURCE.read_text()
    cpp = MOCKS + "\n".join(extract(source, name) for name in (
        "get_from_tangent_matrix_tbn", "get_to_tangent_matrix_tbn", "get_to_tangent_matrix_tbn_hq")) + TEST
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("C++ compiler required")
    with tempfile.TemporaryDirectory() as temp:
        path = pathlib.Path(temp) / "tbn.cc"
        exe = pathlib.Path(temp) / "tbn"
        path.write_text(cpp)
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
