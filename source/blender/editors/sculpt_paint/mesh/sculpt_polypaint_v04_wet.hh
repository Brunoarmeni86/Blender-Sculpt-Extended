/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstddef>
namespace blender::ed::sculpt_paint::color::wet_v04c {
struct Sample {
  std::array<float,3> rgb{};
  size_t count=0;
  bool nearest_fallback=false;
};
/* Sample immutable local colors. At coarse levels a small wet disc can lie
 * between all CCG vertices. Use the nearest eligible local surface sample in
 * that case, instead of silently applying dry paint. Hidden/zero-factor points
 * are never fallback candidates. Ties use physical indices for determinism. */
template<class Influence,class Distance,class Hidden,class Read>
bool sample(const Influence &influence,Distance distance,Hidden hidden,Read read,
            double radius,Sample &result)
{
  result=Sample{};
  if(!std::isfinite(radius)||radius<0)return false;
  double sum[3]={},sum_weight=0;
  int nearest=-1;
  double nearest_distance=std::numeric_limits<double>::infinity();
  for(const auto &entry:influence){
    if(!std::isfinite(entry.second)||entry.second<=0||hidden(entry.first))continue;
    const double d=distance(entry.first);
    if(!std::isfinite(d)||d<0)return false;
    if(d<nearest_distance||(d==nearest_distance&&(nearest<0||entry.first<nearest))){
      nearest=entry.first;nearest_distance=d;
    }
    if(d>radius*radius)continue;
    std::array<float,4> value;
    if(!read(entry.first,value))return false;
    for(int c=0;c<3;c++){
      if(!std::isfinite(value[c]))return false;
      sum[c]+=double(value[c])*entry.second;
    }
    sum_weight+=entry.second;result.count++;
  }
  if(sum_weight==0&&nearest>=0){
    std::array<float,4> value;
    if(!read(nearest,value))return false;
    for(int c=0;c<3;c++){
      if(!std::isfinite(value[c]))return false;
      sum[c]=value[c];
    }
    sum_weight=1;result.count=1;result.nearest_fallback=true;
  }
  if(sum_weight>0)for(int c=0;c<3;c++)result.rgb[c]=float(sum[c]/sum_weight);
  return true;
}
/* A valid previous sample has alpha marker 1. Commit only after publishing the
 * color dab; an aborted dab must not advance the wet history. */
inline bool mix(const Sample &sample,const std::array<float,4> &previous,
                bool first,float persistence,float wet_mix,
                const std::array<float,3> &brush,std::array<float,4> &next,
                std::array<float,3> &paint)
{
  if(!sample.count)return false;
  if(!std::isfinite(persistence)||!std::isfinite(wet_mix))return false;
  const float p=first||previous[3]==0?0:std::clamp(persistence,0.f,1.f);
  const float w=std::clamp(wet_mix,0.f,1.f);
  for(int c=0;c<3;c++){
    if(!std::isfinite(sample.rgb[c])||!std::isfinite(previous[c])||!std::isfinite(brush[c]))return false;
    next[c]=sample.rgb[c]*(1-p)+previous[c]*p;
    paint[c]=brush[c]*(1-w)+next[c]*w;
  }
  next[3]=1;return true;
}
/* Native Mesh factors already contain (1-base_mask). Add only the extra
 * protection represented by the retained HR corner mask at the same vertex. */
inline float cage_factor(float factor,float base_mask,float high_mask,bool hidden)
{
  if(hidden||!std::isfinite(factor)||!std::isfinite(base_mask)||!std::isfinite(high_mask))return 0;
  const float b=std::clamp(base_mask,0.f,1.f),h=std::clamp(high_mask,0.f,1.f);
  if(b>=1||h>=1)return 0;
  return std::clamp(factor*(h>b?(1-h)/(1-b):1),0.f,1.f);
}
}
