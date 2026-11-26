/* Adapted from SculptGL src/editing/tools/Flatten.js
Source blob db26ee3c5f1072e75404a74041724ec791764d45.
MIT License

Copyright (c) 2019 Stéphane GINIER

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once
#include "artistic_geometry_v31d1110.hh"
namespace sculpt_physics::artistic_v31d1110 {
struct PlaneState {
 Vec3 origin, normal;
};
inline PlaneState capture_plane(Vec3 origin,Vec3 normal){
 if(!std::isfinite(h::norm(origin))||!std::isfinite(h::norm(normal))||h::norm(normal)<1e-12)
  throw std::invalid_argument("plane");
 return {origin,h::unit(normal)};
}
// SculptGL radial polynomial, exposed for host selection. Do not apply
// together with native falloff unless explicitly requested.
inline double sculptgl_falloff(double x){
 if(!std::isfinite(x))throw std::invalid_argument("plane falloff");
 x=std::clamp(x,0.,1.);
 return (1-x)*(1-x)*(1+2*x+3*x*x);
}
// Fixed plane for the entire stroke. Only remove protrusions.
// Exponential response is additive in dose on a fixed plane before safety.
inline std::vector<Vec3> scrape_plane(const h::Patch& patch,PlaneState plane,
 Material material,const std::vector<double>& exposure,double radius,double pressure,
 const std::vector<double>* compaction=nullptr,bool fast=false){
 plane=capture_plane(plane.origin,plane.normal);
 if(exposure.size()!=patch.p.size()||(compaction&&compaction->size()!=patch.p.size())||!std::isfinite(radius)||radius<=0)
  throw std::invalid_argument("scrape inputs");
 std::vector<Vec3> desired(patch.p.size());
 for(std::size_t i=0;i<desired.size();++i){
  if(!std::isfinite(exposure[i])||exposure[i]<0)throw std::invalid_argument("scrape exposure");
  if(patch.weight[i]<=0)continue;
  auto response=evaluate(material,pressure,compaction?(*compaction)[i]:0.,1.);
  double height=std::max(0.,h::dot(patch.p[i]-plane.origin,plane.normal));
  double fraction=-std::expm1(-exposure[i]/radius*response.transport);
  desired[i]=plane.normal*(-std::min(height*fraction,radius*.1));
 }
 return fast?accept_fast(patch,std::move(desired)):h::optimize(patch,std::move(desired));
}
}
