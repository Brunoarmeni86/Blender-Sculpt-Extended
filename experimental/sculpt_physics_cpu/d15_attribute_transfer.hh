/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace sculpt_physics::attribute_transfer_d15 {
using Id=std::uint64_t;
using Bary=std::array<double,3>;
using UV=std::array<double,2>;
using Color=std::array<double,4>;
using Chart=std::pair<Id,Id>; // layer, chart
struct UVLayer { Id layer=0,chart=0; std::array<UV,3> values{}; };
struct ColorLayer { Id layer=0; std::array<Color,3> values{}; };
struct Triangle {
  Id id=0,face=0,component=0;
  std::vector<UVLayer> uv;
  std::vector<ColorLayer> point_color,corner_color;
  std::array<double,3> mask{};
  int face_set=1,material=0;
};
struct Snapshot { Id generation=0; bool has_mask=false; std::vector<Triangle> triangles; };
enum class Domain { Point, Corner, Face };
struct Request {
  Domain domain=Domain::Point;
  Id destination=0,generation=0,triangle=0,component=0;
  Bary bary{1.,0.,0.};
  std::vector<Chart> charts; // exact layer/chart membership for a corner
};
struct Record {
  Domain domain=Domain::Point;
  Id destination=0,source_triangle=0,source_face=0;
  std::vector<std::pair<Id,UV>> uv;
  std::vector<std::pair<Id,Color>> colors;
  bool has_mask=false;
  double mask=0.;
  int face_set=1,material=0;
};
struct Batch { Id generation=0; std::vector<Record> records; };
enum class Status { Ok, Invalid, Stale, MissingSource, ChartMismatch, DuplicateDestination };

template<std::size_t N> inline bool finite(const std::array<double,N>& x)
{ for(double v:x)if(!std::isfinite(v))return false;return true; }
inline bool bary_valid(const Bary& w)
{ return finite(w)&&w[0]>=0.&&w[1]>=0.&&w[2]>=0.&&std::abs(w[0]+w[1]+w[2]-1.)<=1e-9; }
template<std::size_t N> inline std::array<double,N> mix(const std::array<std::array<double,N>,3>& x,const Bary& w)
{
  std::array<double,N> r{};
  for(int i=0;i<3;++i)for(std::size_t j=0;j<N;++j)r[j]+=w[i]*x[i][j];
  return r;
}
template<class L> inline bool valid_layers(const std::vector<L>& layers,const std::vector<L>& schema)
{
  if(layers.size()!=schema.size())return false;
  for(std::size_t i=0;i<layers.size();++i){
    if(layers[i].layer!=schema[i].layer||(i&&layers[i-1].layer>=layers[i].layer))return false;
    for(const auto& v:layers[i].values)if(!finite(v))return false;
  }
  return true;
}
// All inputs are read-only snapshots and already-resolved source anchors.
// This function does not search for triangles, modify a mesh, or transfer
// active brush permissions. The caller owns topology, wedge and Undo policy.
inline Status stage(const Snapshot& src,const std::vector<Request>& requests,Batch& out)
{
  if(src.triangles.empty()&&!requests.empty())return Status::MissingSource;
  std::vector<const Triangle*> index;
  index.reserve(src.triangles.size());
  for(const auto& t:src.triangles){
    const auto& schema=src.triangles.front();
    if(t.material<0||!valid_layers(t.uv,schema.uv)||
       !valid_layers(t.point_color,schema.point_color)||!valid_layers(t.corner_color,schema.corner_color))return Status::Invalid;
    if(src.has_mask)for(double m:t.mask)if(!std::isfinite(m)||m<0.||m>1.)return Status::Invalid;
    index.push_back(&t);
  }
  std::sort(index.begin(),index.end(),[](auto a,auto b){return a->id<b->id;});
  for(std::size_t i=1;i<index.size();++i)if(index[i-1]->id==index[i]->id)return Status::Invalid;
  Batch trial;trial.generation=src.generation;trial.records.reserve(requests.size());
  for(const auto& q:requests){
    if(q.generation!=src.generation)return Status::Stale;
    if(!bary_valid(q.bary))return Status::Invalid;
    if(q.domain!=Domain::Point&&q.domain!=Domain::Corner&&q.domain!=Domain::Face)return Status::Invalid;
    const auto it=std::lower_bound(index.begin(),index.end(),q.triangle,[](auto a,Id id){return a->id<id;});
    if(it==index.end()||(*it)->id!=q.triangle||(*it)->component!=q.component)return Status::MissingSource;
    const auto& t=**it;
    Record r;r.domain=q.domain;r.destination=q.destination;r.source_triangle=t.id;r.source_face=t.face;
    if(q.domain==Domain::Corner){
      if(q.charts.size()!=t.uv.size())return Status::ChartMismatch;
      for(std::size_t i=0;i<t.uv.size();++i){
        const auto& layer=t.uv[i];
        if(q.charts[i]!=Chart{layer.layer,layer.chart})return Status::ChartMismatch;
        auto value=mix(layer.values,q.bary);if(!finite(value))return Status::Invalid;
        r.uv.emplace_back(layer.layer,value);
      }
    }else if(!q.charts.empty())return Status::Invalid;
    if(q.domain==Domain::Point||q.domain==Domain::Corner){
      const auto& layers=q.domain==Domain::Point?t.point_color:t.corner_color;
      for(const auto& layer:layers){auto value=mix(layer.values,q.bary);if(!finite(value))return Status::Invalid;r.colors.emplace_back(layer.layer,value);}
    }
    if(q.domain==Domain::Point&&src.has_mask){
      r.has_mask=true;
      for(int i=0;i<3;++i)r.mask+=q.bary[i]*t.mask[i];
      if(!std::isfinite(r.mask))return Status::Invalid;
      r.mask=std::clamp(r.mask,0.,1.);
    }
    if(q.domain==Domain::Face){r.face_set=t.face_set;r.material=t.material;}
    trial.records.push_back(std::move(r));
  }
  std::sort(trial.records.begin(),trial.records.end(),[](const Record& a,const Record& b){
    return a.domain!=b.domain?a.domain<b.domain:a.destination<b.destination;
  });
  for(std::size_t i=1;i<trial.records.size();++i){
    const auto& a=trial.records[i-1];const auto& b=trial.records[i];
    if(a.domain==b.domain&&a.destination==b.destination)return Status::DuplicateDestination;
  }
  out=std::move(trial);return Status::Ok;
}
} // namespace sculpt_physics::attribute_transfer_d15
