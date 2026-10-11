/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cmath>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>
#include "BKE_kelvinlet.h"

namespace blender::ed::sculpt_paint::brushes {
/* SCULPT_ELASTIC_GRAB_EXACT_V1: precompute coefficients, preserve scalar operation order. */
struct PreparedElasticGrab {
  KelvinletParams params;
  float scaled_squared[3];
  float weights[3];
  float denominator;
  int iterations;
  PreparedElasticGrab(const KelvinletParams &p, int n) : params(p), iterations(n) {
    for(int i=0;i<3;i++)scaled_squared[i]=p.radius_scaled[i]*p.radius_scaled[i];
    weights[0]=1.0f;
    if(n==3) {
      weights[1]=-((scaled_squared[2]-scaled_squared[0]) /
                   (scaled_squared[2]-scaled_squared[1]));
      weights[2]=(scaled_squared[1]-scaled_squared[0]) /
                  (scaled_squared[2]-scaled_squared[1]);
      denominator=weights[0]/p.radius_scaled[0]+weights[1]/p.radius_scaled[1]+
                  weights[2]/p.radius_scaled[2];
    }
    else if(n==2) {
      denominator=(1.0f/p.radius_scaled[0])-(1.0f/p.radius_scaled[1]);
    }
    else { denominator=1.0f; }
  }
  float fade_at(const float position[3],const float location[3]) const {
    const float dx=location[0]-position[0];
    const float dy=location[1]-position[1];
    const float dz=location[2]-position[2];
    const float radius=std::sqrt(dx*dx+dy*dy+dz*dz);
    const float r2=radius*radius;
    float k[3];
    for(int i=0;i<iterations;i++) {
      const float re=std::sqrt(r2+scaled_squared[i]);
      const float re3=re*re*re;
      k[i]=((params.a-params.b)/re)+((params.b*r2)/re3)+
           ((params.a*scaled_squared[i])/(2.0f*re3));
    }
    float fade;
    if(iterations==1)fade=k[0]*params.c;
    else if(iterations==2)fade=(k[0]-k[1])*params.c/denominator;
    else {
      const float u=weights[0]*k[0]+weights[1]*k[1]+weights[2]*k[2];
      fade=u*params.c/denominator;
    }
    return fade;
  }
  void translation(float out[3],const float position[3],const float location[3],
                   const float delta[3]) const {
    const float fade=fade_at(position,location);
    out[0]=delta[0]*fade;out[1]=delta[1]*fade;out[2]=delta[2]*fade;
  }
};
/* SCULPT_ELASTIC_GRAB_EXACT_V2: stroke-owned, bounded, lazy scalar weights.
 * Source pointers refer to immutable mesh/grid positions in the active undo step.
 * BMesh/Dyntopo must never enter this cache. Strength, masks and drag remain live. */
struct ElasticGrabWeightCache {
  struct Key {
    const void *source;
    size_t count;
    int iterations;
    std::array<uint32_t,9> bits;
    bool operator==(const Key &other) const {
      return source==other.source && count==other.count && iterations==other.iterations && bits==other.bits;
    }
  };
  struct Hash {
    size_t operator()(const Key &k) const {
      size_t h=std::hash<const void *>{}(k.source);
      auto combine=[&](size_t x){h^=x+size_t(0x9e3779b9)+(h<<6)+(h>>2);};
      combine(k.count);combine(size_t(k.iterations));
      for(uint32_t b:k.bits)combine(b);
      return h;
    }
  };
  struct Entry {
    std::mutex mutex;
    std::vector<float> weights;
    explicit Entry(size_t count):weights(count,std::numeric_limits<float>::quiet_NaN()){}
  };
  std::mutex mutex;
  std::unordered_map<Key,std::shared_ptr<Entry>,Hash> entries;
  const size_t limit;
  size_t used=0;
  explicit ElasticGrabWeightCache(size_t bytes):limit(bytes){}

  std::shared_ptr<Entry> acquire(const void *source,size_t count,
                                 const PreparedElasticGrab &prepared,const float location[3]) {
    Key key{source,count,prepared.iterations,{}};
    const float values[9]={prepared.params.a,prepared.params.b,prepared.params.c,
      prepared.params.radius_scaled[0],prepared.params.radius_scaled[1],prepared.params.radius_scaled[2],
      location[0],location[1],location[2]};
    std::memcpy(key.bits.data(),values,sizeof(values));
    std::lock_guard<std::mutex> lock(mutex);
    const auto found=entries.find(key);
    if(found!=entries.end())return found->second;
    constexpr size_t overhead=512; // Conservative allowance for entry/hash metadata.
    if(entries.size()>=16384 || limit<overhead || count>(limit-overhead)/sizeof(float))return {};
    const size_t bytes=count*sizeof(float)+overhead;
    if(bytes>limit-used)return {};
    try {
      auto entry=std::make_shared<Entry>(count);
      entries.emplace(key,entry);
      used+=bytes;
      return entry;
    }
    catch(const std::bad_alloc &){return {};}
  }
};
} // namespace blender::ed::sculpt_paint::brushes
