/* Adapted from SculptGL src/editing/tools/{Brush,Drag,Pinch,Smooth}.js
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
#include "artistic_plane_v31d1111.hh"
namespace sculpt_physics::artistic_v31d1110 {
enum class SurfaceAction { Clay, Push, Smear, Gather, SmoothTangent, SmoothNormal };
// Exposure is already weighted by host contact/falloff/permission.
// All proposals read the same geometry (Jacobi), no in-place order bias.
inline std::vector<Vec3> surface_target(const h::Patch& patch,SurfaceAction action,
 Material material,PlaneState plane,Vec3 center,Vec3 motion,
 const std::vector<double>& exposure,double radius,double pressure){
 plane=capture_plane(plane.origin,plane.normal);
 if(exposure.size()!=patch.p.size()||!std::isfinite(radius)||radius<=0||
 !std::isfinite(h::norm(center))||!std::isfinite(h::norm(motion)))
  throw std::invalid_argument("surface inputs");
 auto r=evaluate(material,pressure,0,1);
 std::vector<Vec3> out(patch.p.size());
 for(std::size_t i=0;i<out.size();++i){
  const double e=exposure[i];
  if(!std::isfinite(e)||e<0)throw std::invalid_argument("surface exposure");
  if(patch.weight[i]<=0||e==0)continue;
  const auto n=patch.normal[i];
  const auto tangent=[&](Vec3 v){return v-n*h::dot(v,n);};
  const auto fraction=[&](double k){return -std::expm1(-e/radius*k);};
  switch(action){
   case SurfaceAction::Clay:{
    double gap=std::max(0.,radius*.1-h::dot(patch.p[i]-plane.origin,plane.normal));
    out[i]=plane.normal*(gap*fraction(r.deposit));break;
   }
   case SurfaceAction::Push:out[i]=h::unit(tangent(motion))*(e*r.transport);break;
   case SurfaceAction::Smear:out[i]=h::unit(tangent(motion))*(e*r.drag);break;
   case SurfaceAction::Gather:out[i]=tangent(center-patch.p[i])*fraction(r.drag);break;
   case SurfaceAction::SmoothTangent:
   case SurfaceAction::SmoothNormal:{
    Vec3 avg{};double sum=0;
    for(auto j:patch.adj[i])if(patch.weight[j]>0){
     double w=std::max(0.,h::dot(n,patch.normal[j]));
     avg+=(patch.p[j]-patch.p[i])*w;sum+=w;
    }
    if(sum>0){
     avg=avg/sum;
     out[i]=(action==SurfaceAction::SmoothTangent?tangent(avg):n*h::dot(avg,n))*fraction(r.transport);
    }
    break;
   }
  }
  double len=h::norm(out[i]);
  if(len>radius*.1)out[i]=out[i]*(radius*.1/len);
 }
 return h::optimize(patch,std::move(out));
}
}
