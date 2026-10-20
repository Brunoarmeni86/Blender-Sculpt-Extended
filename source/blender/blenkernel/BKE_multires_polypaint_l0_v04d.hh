/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
namespace blender::bke::multires_polypaint_l0_v04d {
inline bool enabled()
{
  const char *v=std::getenv("BLENDER_SCULPT_POLYPAINT_V04");
  const char *display=std::getenv("BLENDER_SCULPT_POLYPAINT_L0_DRAW_V04D");
  return v&&std::strcmp(v,"1")==0&&(!display||std::strcmp(display,"0")!=0);
}
/* Exact area mean of the piecewise bilinear CCG color surface. Boundary
 * trapezoidal weights integrate cells without over-counting shared samples.
 * Display-only restriction: no allocation, resampling or write to the master.
 * Sparse grids use the integral of the same implicit bilinear base patch. */
inline bool restrict_grid(const float (*colors)[4],int level,int count,
                          const float fallback_mean[4],float out[4])
{
  for(int c=0;c<4;c++)out[c]=fallback_mean[c];
  if(!colors||level<1||level>13)return false;
  const int side=(1<<(level-1))+1;
  if(count!=side*side)return false;
  double sum[4]={};
  for(int y=0;y<side;y++){
    const double wy=y==0||y==side-1?.5:1.;
    for(int x=0;x<side;x++){
      const double weight=wy*(x==0||x==side-1?.5:1.);
      for(int c=0;c<4;c++){
        const float value=colors[y*side+x][c];
        if(!std::isfinite(value))return false;
        sum[c]+=double(value)*weight;
      }
    }
  }
  const double area=double(side-1)*(side-1);
  float result[4];
  for(int c=0;c<4;c++){
    result[c]=float(sum[c]/area);
    if(!std::isfinite(result[c]))return false;
  }
  for(int c=0;c<4;c++)out[c]=result[c];
  return true;
}
}
