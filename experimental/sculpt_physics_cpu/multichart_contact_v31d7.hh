/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "material_transport_v21.hh"

namespace sculpt_physics {

/* D7: bounded two-chart fallback, for a steep connected fold inside an
 * existing native footprint. Never widen the host mask. The old single
 * chart remains the exact fast path when no steep region is detected.
 * This is not a visibility raycast, a global atlas, or volumetric physics.
 * A face-normal cone towards the view excludes clearly rear-facing cells;
 * connected occluded folds still require dedicated visibility testing. */
struct MultiChartContactV31D7 {
  std::size_t eligible=0, connected=0, steep=0, charts=0, recovered=0;
  double final_scale=1, signed_volume=0;
};

inline bool solve_multichart_contact_v31d7(
    const std::vector<ClaySurfaceVertex> &vertices,
    const std::vector<ClaySurfaceTriangle> &triangles,
    const ClaySurfaceDab &dab, Vec3 view_normal, int substeps,
    ClaySurfaceResult &result, MultiChartContactV31D7 &stats)
{
  const auto norm=[](Vec3 a){return std::hypot(a.x,a.y,a.z);};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto smooth=[](double t){t=std::clamp(t,0.,1.);return t*t*(3.-2.*t);};
  const double al=norm(dab.axis),vl=norm(view_normal);
  if(!dab.live_surface_v30 || !dab.continuous_contact_v31d2 ||
     dab.plastic_v29 || dab.dose<=0 || dab.intensity<=0 ||
     !std::isfinite(al) || al<=1e-12 || !std::isfinite(vl) || vl<=1e-12 ||
     !std::isfinite(dab.radius) || dab.radius<=1e-12)return false;
  const Vec3 axis=dab.axis/al,view=view_normal/vl;
  if(dot(axis,view)<=.05)return false;
  const std::size_t n=vertices.size();
  std::vector<Vec3> p(n),dual(n),normal(n);
  std::vector<double> area(n),weight(n),distance(n),geodesic(n,std::numeric_limits<double>::infinity());
  std::vector<bool> bad(n,false);
  for(std::size_t i=0;i<n;++i){
    p[i]=vertices[i].position;
    if(!std::isfinite(norm(p[i])) || !std::isfinite(vertices[i].influence) ||
       vertices[i].influence<0 || vertices[i].influence>1)return false;
  }
  for(const auto &t:triangles){
    for(auto i:t)if(i>=n)throw std::out_of_range("D7 triangle index");
    const Vec3 a=cross(p[t[1]]-p[t[0]],p[t[2]]-p[t[0]])/6.;
    const double len=norm(a);
    if(!std::isfinite(len))return false;
    for(auto i:t){dual[i]+=a;area[i]+=len;if(len<=1e-24)bad[i]=true;}
  }
  // Seed at the nearest supported native vertex. 3D bound limits the
  // previously projected footprint, rather than admitting more vertices.
  const double reach=1.5*dab.radius+(dab.swept_v29?norm(dab.sweep_start-dab.center):0.);
  std::size_t seed=n;
  double nearest=reach;
  for(std::size_t i=0;i<n;++i){
    if(vertices[i].influence<=0)continue;
    ++stats.eligible;
    distance[i]=norm(p[i]-dab.center);
    const double a=norm(dual[i]);
    if(bad[i] || area[i]<=1e-24 || a<=.12*area[i] || distance[i]>=reach)continue;
    normal[i]=dual[i]/a;
    if(dot(normal[i],view)<=.035)continue;
    weight[i]=vertices[i].influence;
    if(distance[i]<nearest){nearest=distance[i];seed=i;}
  }
  if(seed==n)return false;
  // Avoid graph allocation on ordinary, nearly single-chart surfaces.
  bool candidate=false;
  for(std::size_t i=0;i<n;++i)if(weight[i]>.02 && dot(normal[i],axis)<.30 &&
      distance[i]<dab.radius){candidate=true;break;}
  if(!candidate)return false;
  std::vector<std::vector<std::pair<std::size_t,double>>> adj(n);
  for(const auto &t:triangles)for(int j=0;j<3;++j){
    const auto a=t[j],b=t[(j+1)%3];
    if(weight[a]<=0 || weight[b]<=0 || dot(normal[a],normal[b])<-.05)continue;
    const double len=norm(p[a]-p[b]);
    if(len<=1e-12 || len>=reach)continue;
    adj[a].push_back({b,len});adj[b].push_back({a,len});
  }
  using Entry=std::pair<double,std::size_t>;
  std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> q;
  geodesic[seed]=nearest;q.push({nearest,seed});
  while(!q.empty()){
    const auto [d,i]=q.top();q.pop();if(d!=geodesic[i])continue;
    for(const auto &e:adj[i]){
      const double next=d+e.second;
      if(next<reach && next<geodesic[e.first]){
        geodesic[e.first]=next;q.push({next,e.first});
      }
    }
  }
  std::size_t steep_seed=n;double best=0;
  for(std::size_t i=0;i<n;++i){
    if(weight[i]<=0 || !std::isfinite(geodesic[i])){weight[i]=0;continue;}
    ++stats.connected;
    const double facing=dot(normal[i],axis);
    if(facing>=.30 || distance[i]>=dab.radius || weight[i]<=.02)continue;
    ++stats.steep;
    const double score=weight[i]*(.30-facing)*dot(normal[i],view)*
        (1.-.5*distance[i]/dab.radius);
    if(score>best){best=score;steep_seed=i;}
  }
  if(steep_seed==n || stats.steep<2)return false;
  // Smooth the secondary axis only within its normal cone. The primary
  // tool axis is unchanged. Do not replace normals at each vertex.
  Vec3 secondary{};
  const Vec3 anchor=normal[steep_seed];
  for(std::size_t i=0;i<n;++i)if(weight[i]>0 && dot(normal[i],anchor)>.75 &&
      norm(p[i]-p[steep_seed])<.45*dab.radius)
    secondary+=normal[i]*(weight[i]*norm(dual[i]));
  const double sl=norm(secondary);
  if(sl<=1e-24)return false;
  secondary=secondary/sl;
  if(dot(secondary,view)<=.05 || dot(secondary,axis)>.65)return false;
  const std::array<Vec3,2> axes={axis,secondary};
  std::array<std::vector<ClaySurfaceVertex>,2> local={vertices,vertices};
  for(std::size_t i=0;i<n;++i){
    double a=0,b=0;
    if(weight[i]>0){
      // Smooth partition of native influence. No extra energy from
      // overlap; very steep normals are assigned only to chart 2.
      a=smooth((dot(normal[i],axis)-.10)/.65);
      b=smooth((dot(normal[i],secondary)-.10)/.65);
      if(dot(normal[i],axis)>.85)b=0;
      const double sum=a+b;
      if(sum>1e-12){a*=weight[i]/sum;b*=weight[i]/sum;}
    }
    local[0][i].influence=a;local[1][i].influence=b;
  }
  ClaySurfaceResult combined;
  combined.displacement.resize(n);combined.projected_area.resize(n);
  std::vector<bool> secondary_motion(n,false);
  for(int chart=0;chart<2;++chart){
    auto d=dab;d.axis=axes[chart];
    d.tangential_gain_v31d6=0; // original D6 is single-chart only
    if(chart==1){
      // Local tool reference at the connected surface, with the same
      // radius and native factors. Swept segment belongs to chart 1.
      d.center=p[steep_seed];d.swept_v29=false;
      if(d.has_tool_plane)d.tool_plane_height=-d.sign*d.tool_depth*d.radius;
    }
    ClaySurfaceResult part=d.extended_material?
        solve_material_transport_v21(local[chart],triangles,d,substeps,false).surface:
        solve_clay_surface(local[chart],triangles,d);
    if(part.active>0)++stats.charts;
    combined.accepted_scale=std::min(combined.accepted_scale,part.accepted_scale);
    combined.slope_scale=std::min(combined.slope_scale,part.slope_scale);
    combined.transport_nodes+=part.transport_nodes;
    combined.trace_attempts+=part.trace_attempts;combined.trace_rejected+=part.trace_rejected;
    combined.transported_volume+=part.transported_volume;
    combined.chart_rejected_v30b+=part.chart_rejected_v30b;
    combined.component_rejected_v30b+=part.component_rejected_v30b;
    for(std::size_t i=0;i<n;++i){
      if(local[chart][i].influence<=0)continue;
      combined.displacement[i]+=part.displacement[i];
      if(chart==1 && dot(normal[i],axis)<.30 && norm(part.displacement[i])>1e-15)
        secondary_motion[i]=true;
    }
  }
  if(std::none_of(secondary_motion.begin(),secondary_motion.end(),[](bool v){return v;}))return false;
  // Guard the SUM against the original incident 3D triangles. Each chart
  // has already enforced its own signed projected budget. The combined
  // first-order swept volume is measured on real dual vectors, not
  // assumed to be an exact finite-step conservation law.
  double peak=0;
  for(const auto &m:combined.displacement)peak=std::max(peak,norm(m));
  double scale=peak>.08*dab.radius?.08*dab.radius/peak:1.;
  scale=std::min(scale,gradient_motion_scale_v31c(p,triangles,combined.displacement));
  if(scale<=0)return false;
  if(scale<1)for(auto &m:combined.displacement)m=m*scale;
  const auto guard=limit_local_contact(p,triangles,combined.displacement,true);
  stats.final_scale=scale*guard.final_scale;
  if(stats.final_scale<=0)return false;
  combined.accepted_scale*=stats.final_scale;
  combined.slope_scale=std::min(combined.slope_scale,scale);
  combined.local_limited_v30b=guard.limited_vertices;
  combined.patches_v30c=stats.charts;
  combined.transported_volume*=stats.final_scale;
  for(std::size_t i=0;i<n;++i){
    combined.projected_area[i]=std::max(0.,dot(dual[i],axis));
    if(norm(combined.displacement[i])<=1e-15)continue;
    ++combined.active;
    if(secondary_motion[i])++stats.recovered;
    stats.signed_volume+=dot(dual[i],combined.displacement[i]);
  }
  if(!stats.recovered)return false;
  combined.signed_swept_volume=stats.signed_volume;
  result=std::move(combined);
  return true;
}
} // namespace sculpt_physics
