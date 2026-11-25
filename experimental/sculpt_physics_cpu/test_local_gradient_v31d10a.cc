/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "brush_contact.hh"
#include <cstdlib>
#include <iostream>
using namespace sculpt_physics;
static void require(bool ok,const char *what)
{
  if(!ok){std::cerr<<what<<'\n';std::exit(EXIT_FAILURE);}
}
static double length(Vec3 v){return std::hypot(v.x,v.y,v.z);}
int main()
{
  // Two broad patches with equal opposite normal motion, one narrow
  // triangle with a local spike, and an immovable halo.
  std::vector<Vec3> p{{0,0,0},{1,0,0},{0,1,0},
                     {2,0,0},{3,0,0},{2,1,0},
                     {4,0,0},{5,0,0},{4,.001,0}};
  std::vector<std::array<std::size_t,3>> t{{0,1,2},{3,4,5},{6,7,8}};
  std::vector<Vec3> original{{0,0,.05},{0,0,.05},{},
                             {0,0,-.05},{0,0,-.05},{},
                             {},{},{0,0,.01}};
  auto motion=original;
  const auto local=limit_local_gradient_v31d10a(p,t,motion);
  require(local.used_local,"local guard must recover broad patches");
  require(local.baseline_scale<.03,"narrow triangle must constrain global baseline");
  require(local.retained_ratio>.9,"retain broad positive and negative flux");
  require(gradient_motion_scale_v31c(p,t,motion)>=1-1e-10,"gradient safety");
  for(const auto &tri:t){
    ContactTriangle c{};
    for(int j=0;j<3;++j){c.position[j]=p[tri[j]];c.motion[j]=motion[tri[j]];}
    require(contact_triangle_scale(c)>=1-1e-10,"triangle safety");
  }
  for(std::size_t i=0;i<motion.size();++i){
    require(length(motion[i])<=length(original[i])+1e-15,"no added motion");
    if(length(original[i])==0)require(length(motion[i])==0,"pinned unchanged");
  }
  // Oriented dual area: each vertex owns one third of triangle area.
  const double pos_before=(.05+.05)/6+.01*.001/6;
  const double neg_before=(.05+.05)/6;
  const double pos_after=(motion[0].z+motion[1].z)/6+motion[8].z*.001/6;
  const double neg_after=-(motion[3].z+motion[4].z)/6;
  require(std::abs(pos_after/pos_before-neg_after/neg_before)<1e-12,"signed flux ratio");
  // Already safe: exact original response, no local intervention.
  auto safe=original;safe[8]={};
  const auto before=safe;
  const auto unchanged=limit_local_gradient_v31d10a(p,t,safe);
  require(!unchanged.used_local,"safe field follows baseline");
  for(std::size_t i=0;i<safe.size();++i)
    require(length(safe[i]-before[i])==0,"safe baseline unchanged");
  // Single unsafe face: local headroom gives no benefit; exact fallback.
  std::vector<Vec3> q{p[6],p[7],p[8]},m{original[6],original[7],original[8]};
  std::vector<std::array<std::size_t,3>> one{{0,1,2}};
  const auto scale=gradient_motion_scale_v31c(q,one,m);
  auto baseline=m;
  for(auto &v:baseline)v=v*scale;
  limit_local_contact(q,one,baseline,true);
  const auto fallback=limit_local_gradient_v31d10a(q,one,m);
  require(!fallback.used_local,"non-improving local guard must fall back");
  for(std::size_t i=0;i<m.size();++i)
    require(length(m[i]-baseline[i])==0,"fallback equals original guard");
  std::cout<<"D10A local gradient tests passed\n";
}
