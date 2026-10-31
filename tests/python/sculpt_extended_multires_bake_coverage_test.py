"""Extract CPU raster/flush code and test it with mocked shading and image services.

Validates UV sample coverage, reference selection and concurrent ownership.
Does not compile Blender or validate its CCG sampling, MikkTSpace or padding.
Use --source FILE --expect-baseline to reproduce the old center-only culling.
"""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def extract(source, name):
    # Find the beginning of this definition, not a call.
    lines = source[: source.index(name + "(")].splitlines()
    signature = lines[-1]
    start = source.rfind(signature, 0, source.index(name + "(") + 1)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MOCKS = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
struct float2 {
  float x=0,y=0;
  float2()=default;float2(float a,float b):x(a),y(b){};
  float2 &operator+=(float2 b){x+=b.x;y+=b.y;return *this;}
};
float2 operator+(float2 a,float2 b){return {a.x+b.x,a.y+b.y};}
float2 operator-(float2 a,float2 b){return {a.x-b.x,a.y-b.y};}
float2 operator-(float2 a,float b){return {a.x-b,a.y-b};}
float2 operator*(float2 a,float2 b){return {a.x*b.x,a.y*b.y};}
float2 operator*(float2 a,float b){return {a.x*b,a.y*b};}
float2 operator/(float2 a,float b){return {a.x/b,a.y/b};}
struct float3 {
  float x=0,y=0,z=0;
  float3()=default;explicit float3(float a):x(a),y(a),z(a){}
  float3(float a,float b,float c):x(a),y(b),z(c){}
  float3 &operator+=(float3 b){x+=b.x;y+=b.y;z+=b.z;return *this;}
};
struct int2 {int x,y;int2(int a,int b):x(a),y(b){}};
namespace math {float cross(float2 a,float2 b){return a.x*b.y-a.y*b.x;}}
float2 resolve_tri_uv(float2 p,float2 a,float2 b,float2 c){
  const float2 e=b-a,f=c-a,d=p-a;const float det=math::cross(e,f);
  const float bw=math::cross(d,f)/det,cw=math::cross(e,d)/det;
  return {1-bw-cw,bw};
}
float2 interp_barycentric_triangle(const float2 *p,float2 b){
  return p[0]*b.x+p[1]*b.y+p[2]*(1-b.x-b.y);
}
template<class T> struct Buffer:std::vector<T>{using std::vector<T>::vector;bool is_empty()const{return this->empty();}};
constexpr char FILTER_MASK_NULL=0,FILTER_MASK_USED=1;
using SpinLock=std::mutex;
void BLI_spin_lock(SpinLock *s){s->lock();}void BLI_spin_unlock(SpinLock *s){s->unlock();}
struct CoverageLockPool {
  std::array<SpinLock,4096> locks;
  SpinLock &for_pixel(int64_t p){return locks[size_t(p)&4095];}
};
struct ImBuf{int x,y;std::vector<float3> pixels;};
struct ExtraBuffers{Buffer<char> mask_buffer;Buffer<uint64_t> quality_pixel_owner;};
struct RasterizeTile{ImBuf *ibuf;ExtraBuffers *extra_buffers;CoverageLockPool *coverage_locks;};
struct RasterizeTriangle{uint32_t raster_index=0;int grid_index=0;float2 tex_uvs[3],grid_uvs[3];};
struct RasterizeResult{};
int quality_side=4;
int multires_bake_quality_sample_side(){return quality_side;}
struct MultiresBaker {
  bool supports_quality_supersampling()const{return true;}
  bool supports_quality_coverage()const{return false;}
  float3 bake_pixel(const RasterizeTriangle &t,float2,float2,RasterizeResult &)const{return float3(float(t.grid_index));}
  float3 bake_quality_sample(const RasterizeTriangle &t,float2 b,float2,RasterizeResult &)const{return {float(t.grid_index),b.x,b.y};}
  float3 finalize_quality_samples_at_pixel(const RasterizeTriangle &t,float2 b,float3,int)const{return {float(t.grid_index),b.x,b.y};}
  void write_pixel(const RasterizeTile &t,int2 p,float3 v)const{t.ibuf->pixels[p.y*t.ibuf->x+p.x]=v;}
};
void rasterize_triangle_center_reference(const MultiresBaker &,const RasterizeTile &,const RasterizeTriangle &,RasterizeResult &){}
void rasterize_triangle_coverage(const MultiresBaker &,const RasterizeTile &,const RasterizeTriangle &,float2,int,RasterizeResult &){}
'''

TESTS = r'''
struct Fixture {
  ImBuf image;ExtraBuffers buffers;CoverageLockPool locks;RasterizeTile tile;MultiresBaker baker;
  Fixture(int w=8,int h=8,bool owned=true):image{w,h,std::vector<float3>(w*h)},tile{&image,&buffers,&locks}{
    buffers.mask_buffer.assign(w*h,FILTER_MASK_NULL);
    if(owned)buffers.quality_pixel_owner.assign(w*h,std::numeric_limits<uint64_t>::max());
  }
  void raster(const RasterizeTriangle &t){RasterizeResult r;rasterize_triangle(baker,tile,t,r);}
};
RasterizeTriangle tri(uint32_t id,int tag,std::array<float2,3> pixels,int w=8,int h=8){
  RasterizeTriangle t;t.raster_index=id;t.grid_index=tag;
  for(int i=0;i<3;++i){t.tex_uvs[i]={pixels[i].x/w,pixels[i].y/h};t.grid_uvs[i]=t.tex_uvs[i];}
  return t;
}
bool inside_double(const RasterizeTriangle &t,double x,double y,double *edge_distance=nullptr){
  double ex=t.tex_uvs[1].x-t.tex_uvs[0].x,ey=t.tex_uvs[1].y-t.tex_uvs[0].y;
  double fx=t.tex_uvs[2].x-t.tex_uvs[0].x,fy=t.tex_uvs[2].y-t.tex_uvs[0].y;
  double dx=x-t.tex_uvs[0].x,dy=y-t.tex_uvs[0].y,det=ex*fy-ey*fx;
  if(det==0){if(edge_distance)*edge_distance=1;return false;}
  double b=(dx*fy-dy*fx)/det,c=(ex*dy-ey*dx)/det,a=1-b-c;
  if(edge_distance)*edge_distance=std::min({std::abs(a),std::abs(b),std::abs(c)});
  return a>=0&&b>=0&&c>=0;
}
bool covers(const RasterizeTriangle &t,int x,int y,int w,int h,bool *ambiguous=nullptr){
  bool covered=false;double d=0;
  covered|=inside_double(t,(x+.5)/w,(y+.5)/h,&d);
  if(ambiguous)*ambiguous|=d<1e-5;
  for(int sy=0;sy<quality_side;++sy)for(int sx=0;sx<quality_side;++sx){
    covered|=inside_double(t,(x+(sx+.5)/quality_side)/w,(y+(sy+.5)/quality_side)/h,&d);
    if(ambiguous)*ambiguous|=d<1e-5;
  }
  return covered;
}
void same_image(const Fixture &a,const Fixture &b){
  assert(a.buffers.mask_buffer==b.buffers.mask_buffer);
  assert(a.buffers.quality_pixel_owner==b.buffers.quality_pixel_owner);
  assert(std::memcmp(a.image.pixels.data(),b.image.pixels.data(),a.image.pixels.size()*sizeof(float3))==0);
}
int main(int argc,char **){
  const bool baseline=argc>1;
  auto corner=tri(0,7,{{{0,0},{.4f,0},{0,.4f}}});
  {Fixture f(8,8,!baseline);f.raster(corner);
   if(baseline){assert(f.buffers.mask_buffer[0]==FILTER_MASK_NULL);std::puts("REPRODUCED: baseline omits a texel with a covered 4x4 sample and uncovered center.");return 0;}
   assert(f.buffers.mask_buffer[0]==FILTER_MASK_USED&&f.image.pixels[0].x==7);
   assert(barycentric_inside_triangle({f.image.pixels[0].y,f.image.pixels[0].z}));}
  auto center=tri(9,9,{{{.1f,.1f},{.9f,.1f},{.5f,.9f}}});
  {Fixture a,b;a.raster(corner);a.raster(center);b.raster(center);b.raster(corner);same_image(a,b);assert(a.image.pixels[0].x==9);}
  // A tiny triangle containing the center retains the existing center fallback.
  {Fixture f;auto t=tri(0,3,{{{.45f,.45f},{.55f,.45f},{.5f,.55f}}});f.raster(t);assert(f.image.pixels[0].x==3);}
  // Conservative candidate visitation must not extrapolate when no sample is covered.
  {Fixture f;auto t=tri(0,3,{{{.01f,.01f},{.02f,.01f},{.01f,.02f}}});f.raster(t);assert(f.buffers.mask_buffer[0]==FILTER_MASK_NULL);}
  std::mt19937 rng(20261012);int checked=0;
  for(int side:{2,4,8}){
    quality_side=side;
    for(int test=0;test<240;++test){
      std::uniform_real_distribution<float> coord(-2,10);
      auto t=tri(uint32_t(test),test+1,{{{coord(rng),coord(rng)},{coord(rng),coord(rng)},{coord(rng),coord(rng)}}});
      Fixture f;f.raster(t);
      for(int y=0;y<8;++y)for(int x=0;x<8;++x){
        bool ambiguous=false;const bool expected=covers(t,x,y,8,8,&ambiguous);
        if(!ambiguous){assert((f.buffers.mask_buffer[y*8+x]!=FILTER_MASK_NULL)==expected);++checked;}
        if(f.buffers.mask_buffer[y*8+x]!=FILTER_MASK_NULL)assert(barycentric_inside_triangle({f.image.pixels[y*8+x].y,f.image.pixels[y*8+x].z}));
      }
      std::swap(t.tex_uvs[0],t.tex_uvs[2]);std::swap(t.grid_uvs[0],t.grid_uvs[2]);
      Fixture mirrored;mirrored.raster(t);assert(f.buffers.mask_buffer==mirrored.buffers.mask_buffer);
    }
  }
  quality_side=4;
  std::vector<RasterizeTriangle> triangles;
  for(int i=0;i<48;++i){float d=float(i%7)*.025f;triangles.push_back(tri(uint32_t(i),i+1,{{{d,d},{7.8f,d},{d,7.8f}}}));}
  Fixture expected;for(auto &t:triangles)expected.raster(t);
  for(int repeat=0;repeat<24;++repeat){
    std::shuffle(triangles.begin(),triangles.end(),rng);
    Fixture parallel;std::atomic<size_t> next{0};std::vector<std::thread> workers;
    for(int worker=0;worker<8;++worker)workers.emplace_back([&]{for(;;){auto i=next.fetch_add(1);if(i>=triangles.size())break;parallel.raster(triangles[i]);}});
    for(auto &worker:workers){worker.join();}
    same_image(expected,parallel);
  }
  // Exact seam at the shared diagonal must have a stable owner in either order.
  {auto a=tri(0,1,{{{0,0},{8,0},{8,8}}}),b=tri(1,2,{{{8,8},{0,8},{0,0}}});
   Fixture f,g;f.raster(a);f.raster(b);g.raster(b);g.raster(a);same_image(f,g);
   for(char mask:f.buffers.mask_buffer)assert(mask==FILTER_MASK_USED);}
  // Finite distant UVs, non-finite coordinates and degenerate triangles are safe.
  {Fixture f;auto t=tri(0,1,{{{1e30f,1e30f},{2e30f,1e30f},{1e30f,2e30f}}});f.raster(t);
   t.tex_uvs[0].x=std::numeric_limits<float>::quiet_NaN();f.raster(t);
   t=tri(0,1,{{{0,0},{4,4},{8,8}}});f.raster(t);
   for(char mask:f.buffers.mask_buffer)assert(mask==FILTER_MASK_NULL);}
  std::printf("PASS: %d coverage comparisons; 720 mirrored triangles; 24 eight-worker overlap permutations; valid references and seam checks.\n",checked);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2] / "source/blender/render/intern/multires_bake.cc")
    parser.add_argument("--expect-baseline", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text()
    names = ["barycentric_inside_triangle", "flush_pixel", "set_rast_triangle", "rasterize_half"]
    if "static void rasterize_triangle_quality_pixels(" in source:
        names.append("rasterize_triangle_quality_pixels")
    names.append("rasterize_triangle")
    cpp = MOCKS + "\n".join(extract(source, name) for name in names) + TESTS
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("C++ compiler required")
    with tempfile.TemporaryDirectory() as temp:
        file = pathlib.Path(temp) / "coverage.cc"
        exe = pathlib.Path(temp) / "coverage"
        file.write_text(cpp)
        flags = ["-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-O1"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([compiler, *flags, str(file), "-o", str(exe)], check=True)
        subprocess.run([str(exe)] + (["baseline"] if args.expect_baseline else []), check=True)


if __name__ == "__main__":
    main()
