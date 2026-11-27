/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "d12_brush_common.hh"
#include <array>
#include <limits>
#include <memory>

namespace sculpt_physics::dedicated_d12::smooth_multiscale {
constexpr int side=33, cells=side*side;
constexpr double grid_scale=12.;
struct Sample {Vec3 p{},normal{};double weight=0.,area=0.;};
struct Point {double x=0.,y=0.,h=0.,mass=0.,weight=0.;};
struct Chart {Vec3 axis{};double radius=1.;std::vector<Point> points;};
// Local height chart, affine detrended. Caller supplies a connected native
// writable region and area proxy. Reject folded sheets instead of blending.
inline bool prepare(const std::vector<Sample>& s,Vec3 center,double radius,Chart& c){
 if(s.size()<12||s.size()>500000||!finite(center)||!std::isfinite(radius)||radius<=0)return false;
 Vec3 normal{};double max_area=0.;
 for(const auto& v:s){
  if(!finite(v.p)||!finite(v.normal)||!std::isfinite(v.weight)||v.weight<=0||v.weight>1||
     !std::isfinite(v.area)||v.area<=0)return false;
  max_area=std::max(max_area,v.area);
 }
 for(const auto& v:s)normal+=unit(v.normal)*(v.area/max_area*v.weight);
 const Vec3 n=unit(normal);
 if(norm2(n)<.5)return false;
 const Vec3 u=unit(cross(std::abs(n.z)<.9?Vec3{0,0,1}:Vec3{0,1,0},n)),v=cross(n,u);
 std::array<double,cells> low,high;
 low.fill(std::numeric_limits<double>::infinity());high.fill(-std::numeric_limits<double>::infinity());
 c.axis=n;c.radius=radius;c.points.clear();c.points.reserve(s.size());
 double w=0.,x=0.,y=0.,h=0.,xx=0.,xy=0.,yy=0.,xh=0.,yh=0.;
 for(const auto& a:s){
  if(dot(unit(a.normal),n)<.40)return false;
  const auto q=(a.p-center)/radius;
  const double px=dot(q,u),py=dot(q,v),z=dot(q,n);
  if(std::abs(px)>1.24||std::abs(py)>1.24||std::abs(z)>.75)return false;
  const int ix=int(std::round(px*grid_scale))+16,iy=int(std::round(py*grid_scale))+16;
  const int id=iy*side+ix;
  low[id]=std::min(low[id],z);high[id]=std::max(high[id],z);
  if(high[id]-low[id]>.24)return false;
  const double mass=a.area/max_area*a.weight;
  c.points.push_back({px,py,z,mass,a.weight});
  w+=mass;x+=mass*px;y+=mass*py;h+=mass*z;
  xx+=mass*px*px;xy+=mass*px*py;yy+=mass*py*py;xh+=mass*px*z;yh+=mass*py*z;
 }
 if(w<=1e-12)return false;
 const double a=xx-x*x/w,b=xy-x*y/w,d=yy-y*y/w;
 const double e=xh-x*h/w,f=yh-y*h/w,det=a*d-b*b;
 if(det<=1e-10*w*w)return false;
 const double bx=(d*e-b*f)/det,by=(a*f-b*e)/det,base=(h-bx*x-by*y)/w;
 for(auto& q:c.points)q.h-=base+bx*q.x+by*q.y;
 return true;
}
inline double grid_x(double x){return x*grid_scale+16.;}
inline void splat(const Point& p,std::array<double,cells>& mass,std::array<double,cells>& height){
 const double x=grid_x(p.x),y=grid_x(p.y);
 const int ix=int(x),iy=int(y);
 for(int j=0;j<2;++j)for(int i=0;i<2;++i){
  const double w=p.mass*(i?x-ix:1.-(x-ix))*(j?y-iy:1.-(y-iy));
  const int id=(iy+j)*side+ix+i;
  mass[id]+=w;height[id]+=w*p.h;
 }
}
inline void filter(const std::array<double,cells>& mass,const std::array<double,cells>& h,
                   std::array<double,cells>& result,std::array<double,cells>& support){
 for(int y=0;y<side;++y)for(int x=0;x<side;++x){
  double m1=0.,h1=0.,m2=0.,h2=0.;
  for(int j=-5;j<=5;++j)for(int i=-5;i<=5;++i){
   const int a=x+i,b=y+j;
   if(a<0||a>=side||b<0||b>=side)continue;
   const int id=b*side+a;
   const double w2=double(6-std::abs(i))*double(6-std::abs(j));
   m2+=mass[id]*w2;h2+=h[id]*w2;
   if(std::abs(i)<=2&&std::abs(j)<=2){
    const double w1=double(3-std::abs(i))*double(3-std::abs(j));
    m1+=mass[id]*w1;h1+=h[id]*w1;
   }
  }
  const int id=y*side+x;
  support[id]=m2>1e-15?1.:0.;
  const double broad=m2>1e-15?h2/m2:0.;
  result[id]=.25*(m1>1e-15?h1/m1:broad)+.75*broad;
 }
}
inline double sample_delta(const Point& p,const std::array<double,cells>& field,const std::array<double,cells>& support){
 const double x=grid_x(p.x),y=grid_x(p.y);
 const int ix=int(x),iy=int(y);
 double val=0.,w=0.;
 for(int j=0;j<2;++j)for(int i=0;i<2;++i){
  const int id=(iy+j)*side+ix+i;
  const double k=support[id]*(i?x-ix:1.-(x-ix))*(j?y-iy:1.-(y-iy));
  val+=k*field[id];w+=k;
 }
 if(w<=1e-15)return 0.;
 return std::clamp((val/w-p.h)*.85,-.055,.055)*p.weight;
}
inline std::vector<double> cpu(const Chart& c){
 std::array<double,cells> mass{},height{},field{},support{};
 for(const auto& p:c.points)splat(p,mass,height);
 filter(mass,height,field,support);
 std::vector<double> result;result.reserve(c.points.size());
 for(const auto& p:c.points)result.push_back(sample_delta(p,field,support));
 return result;
}
class Backend {
 public:
 virtual ~Backend()=default;
 virtual bool evaluate(const Chart&,std::vector<double>&)=0;
};
inline bool validate(const Chart& c,const std::vector<double>& d){
 if(d.size()!=c.points.size())return false;
 for(std::size_t i=0;i<d.size();++i)
  if(!std::isfinite(d[i])||std::abs(d[i])>.055*c.points[i].weight+1e-6)return false;
 return true;
}
// Null if Warp not compiled or its optional multiscale module is unavailable.
std::unique_ptr<Backend> create_cuda();
}
