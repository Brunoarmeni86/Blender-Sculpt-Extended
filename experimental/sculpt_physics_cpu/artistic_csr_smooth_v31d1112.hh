/* Adapted from SculptCore source/brush/kernels/smooth.sbrush.
Copyright (c) 2022 Joseph Eagar

Permission is hereby granted, free of charge, to any person obtaining a copy of this 
software and associated documentation files (the "Software"), to deal in the Software 
without restriction, including without limitation the rights to use, copy, modify, 
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to 
permit persons to whom the Software is furnished to do so, subject to the following 
conditions:

The above copyright notice and this permission notice shall be included in all copies 
or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF 
CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR 
THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#pragma once
#include "artistic_workset_v31d1112.hh"
namespace sculpt_physics::artistic_v31d1110 {
// One independent output per vertex, suitable for a Warp kernel.
// Host workset is immutable. No GPU execution in this reference.
inline std::vector<Vec3> csr_smooth(const ArtisticWorkset& w,Material material,
 double radius,double pressure,bool tangent_only){
 const auto n=w.px.size();
 if(!std::isfinite(radius)||radius<=0||w.offsets.size()!=n+1||
 w.py.size()!=n||w.pz.size()!=n||w.nx.size()!=n||w.ny.size()!=n||
 w.nz.size()!=n||w.weight.size()!=n||w.exposure.size()!=n||
 w.offsets.front()!=0||w.offsets.back()!=w.neighbors.size())
  throw std::invalid_argument("CSR layout");
 auto response=evaluate(material,pressure,0,1);
 std::vector<Vec3> out(n);
 for(std::size_t i=0;i<n;++i){
  if(w.offsets[i]>w.offsets[i+1]||w.offsets[i+1]>w.neighbors.size())
   throw std::invalid_argument("CSR offsets");
  if(w.weight[i]<=0||w.exposure[i]<=0)continue;
  Vec3 p{w.px[i],w.py[i],w.pz[i]},normal=h::unit({w.nx[i],w.ny[i],w.nz[i]});
  Vec3 avg{};double sum=0;
  for(auto k=w.offsets[i];k<w.offsets[i+1];++k){
   auto j=w.neighbors[k];if(j>=n)throw std::invalid_argument("CSR index");
   if(w.weight[j]<=0)continue;
   double a=std::max(0.,h::dot(normal,h::unit({w.nx[j],w.ny[j],w.nz[j]})));
   avg+=(Vec3{w.px[j],w.py[j],w.pz[j]}-p)*a;sum+=a;
  }
  if(sum<=0)continue;
  avg=avg/sum;
  if(tangent_only)avg=avg-normal*h::dot(avg,normal);
  out[i]=avg*(-std::expm1(-w.exposure[i]/radius*response.transport));
  double len=h::norm(out[i]);if(len>radius*.1)out[i]=out[i]*(radius*.1/len);
 }
 return out;
}
}
