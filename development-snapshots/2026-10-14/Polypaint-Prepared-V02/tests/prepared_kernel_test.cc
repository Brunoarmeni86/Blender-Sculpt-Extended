#ifdef NDEBUG
#error Compile this validation harness with assertions enabled
#endif
#include "polypaint_cpu_math.hh"
#include "BKE_multires_polypaint_prepared.hh"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>
using namespace blender::bke::multires_polypaint_cpu;
using Color=std::array<float,4>;
static bool reference(std::vector<Color>&a,int ms,int ps,int x,int y,const float primary[3],float influence){
 if(influence<=0.f)return false;
 auto*p=reinterpret_cast<float(*)[4]>(a.data());float before[4],current[4],target[4],delta[4];
 float u=float(x)/float(ps-1),v=float(y)/float(ps-1);
 if(!sample_grid(p,ms,u,v,before))return false;
 for(int c=0;c<3;c++)target[c]=before[c]*(1.f-influence)+primary[c]*influence;target[3]=before[3];
 for(float f:target)if(!std::isfinite(f))return false;
 if(!sample_grid(p,ms,u,v,current))return false;
 for(int c=0;c<4;c++)delta[c]=target[c]-current[c];
 return add_coarse_delta(p,ms,ps,x,y,delta);
}
static bool equal(const std::vector<Color>&a,const std::vector<Color>&b){
 if(a.size()!=b.size())return false;
 for(size_t i=0;i<a.size();i++)for(int c=0;c<4;c++)if(a[i][c]!=b[i][c]&&std::memcmp(&a[i][c],&b[i][c],sizeof(float)))return false;
 return true;
}
int main(int argc,char**){
 std::mt19937 rng(34017);std::uniform_real_distribution<float> rgb(-2.f,4.f),flow(.0001f,1.f);
 int cases=0;long long updates=0;
 for(int ml=1;ml<=8;ml++)for(int pl=1;pl<=ml;pl++)for(int kind=0;kind<3;kind++){
  int ms=(1<<(ml-1))+1,ps=(1<<(pl-1))+1;
  std::vector<Color>a(ms*ms),b;
  for(auto&c:a)c={rgb(rng),rgb(rng),rgb(rng),.37f};b=a;
  PreparedPaintPlan plan;assert(plan.prepare(ms,ps));
  for(int k=0;k<600;k++){
   int x=k%5==0?0:k%5==1?ps-1:int(rng()%ps),y=k%7==0?0:k%7==1?ps-1:int(rng()%ps);
   float primary[3]={rgb(rng),rgb(rng),rgb(rng)};float influence=kind==0?.5f:kind==1?1.f:flow(rng);
   bool one=reference(a,ms,ps,x,y,primary,influence);
   bool two=plan.blend(reinterpret_cast<float(*)[4]>(b.data()),x,y,primary,influence);
   assert(one==two);updates++;
  }
  assert(equal(a,b));cases++;
 }
 // Non-finite neighbor reads and zero-weight support: follow the legacy rejection path.
 for(int where=0;where<9;where++)for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
  std::vector<Color>a(9,Color{.1f,.3f,.7f,.5f});a[where][0]=bad;auto b=a;PreparedPaintPlan plan;assert(plan.prepare(3,3));float primary[3]={.3f,.2f,.1f};
  assert(reference(a,3,3,0,0,primary,.5f)==plan.blend(reinterpret_cast<float(*)[4]>(b.data()),0,0,primary,.5f));assert(equal(a,b));cases++;
 }
 {PreparedPaintPlan p;assert(!p.prepare(4,3));assert(!p.prepare(3,5));assert(!p.prepare(0,0));assert(p.prepare(3,3));float values[9][4]{};float color[3]={1,1,1};assert(!p.blend(values,-1,0,color,.5f));assert(!p.blend(values,3,0,color,.5f));assert(!p.blend(nullptr,0,0,color,.5f));assert(!p.blend(values,0,0,color,0.f));assert(!p.blend(values,0,0,color,std::numeric_limits<float>::quiet_NaN()));cases++;}
 std::printf("PASS %d kernel cases, %lld sequential updates: same/coarse levels, edges, repeated samples, HDR, alpha, invalid metadata and non-finite neighbors\n",cases,updates);
 if(argc>1){
  for(auto sizes:{std::array<int,2>{129,129},std::array<int,2>{129,65},std::array<int,2>{129,17}}){
   int ms=sizes[0],ps=sizes[1],count=150000;std::vector<Color>base(ms*ms,Color{.2f,.3f,.4f,.7f});float primary[3]={.8f,.1f,.2f};
   std::vector<std::array<int,2>>points(count);for(auto&p:points)p={int(rng()%ps),int(rng()%ps)};
   auto a=base,b=base;PreparedPaintPlan plan;assert(plan.prepare(ms,ps));
   auto start=std::chrono::steady_clock::now();for(auto p:points)reference(a,ms,ps,p[0],p[1],primary,.3f);
   auto middle=std::chrono::steady_clock::now();for(auto p:points)plan.blend(reinterpret_cast<float(*)[4]>(b.data()),p[0],p[1],primary,.3f);
   auto end=std::chrono::steady_clock::now();assert(equal(a,b));
   double oldms=std::chrono::duration<double,std::milli>(middle-start).count(),newms=std::chrono::duration<double,std::milli>(end-middle).count();
   std::printf("ISOLATED_KERNEL master=%d paint=%d samples=%d reference=%.3fms prepared=%.3fms ratio=%.2fx (not Blender end-to-end)\n",ms,ps,count,oldms,newms,oldms/newms);
  }
 }
}
