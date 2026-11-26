#pragma once
#include "artistic_surface_v31d1111.hh"
#include <cstdint>
namespace sculpt_physics::artistic_v31d1110 {
// Host-side staging only. No GPU dispatch or device allocation.
struct ArtisticWorkset {
 std::vector<float> px,py,pz,nx,ny,nz,weight,area,exposure;
 std::vector<std::uint32_t> offsets,neighbors;
};
inline ArtisticWorkset pack_workset(const h::Patch& patch,const std::vector<double>& exposure){
 const auto n=patch.p.size();
 if(n>8192||exposure.size()!=n||patch.adj.size()!=n||
 patch.normal.size()!=n||patch.weight.size()!=n||patch.area.size()!=n)
  throw std::invalid_argument("workset shape");
 ArtisticWorkset w;
 auto narrow=[](double x){
  if(!std::isfinite(x)||std::abs(x)>std::numeric_limits<float>::max())
   throw std::invalid_argument("workset value");
  return static_cast<float>(x);
 };
 w.offsets.push_back(0);
 for(std::size_t i=0;i<n;++i){
  auto p=patch.p[i],v=patch.normal[i];
  w.px.push_back(narrow(p.x));w.py.push_back(narrow(p.y));w.pz.push_back(narrow(p.z));
  w.nx.push_back(narrow(v.x));w.ny.push_back(narrow(v.y));w.nz.push_back(narrow(v.z));
  w.weight.push_back(narrow(patch.weight[i]));w.area.push_back(narrow(patch.area[i]));
  if(exposure[i]<0)throw std::invalid_argument("negative exposure");
  w.exposure.push_back(narrow(exposure[i]));
  for(auto j:patch.adj[i]){
   if(j>=n)throw std::invalid_argument("workset neighbor");
   w.neighbors.push_back(static_cast<std::uint32_t>(j));
  }
  w.offsets.push_back(static_cast<std::uint32_t>(w.neighbors.size()));
 }
 return w;
}
}
