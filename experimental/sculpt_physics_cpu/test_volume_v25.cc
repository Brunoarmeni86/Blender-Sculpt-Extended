/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "volume_session_v25.hh"
#ifdef NDEBUG
# undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace sculpt_physics;

static VolumeSurfaceV25 cube(){
  return {{{0,0,0},{1,0,0},{1,1,0},{0,1,0},
           {0,0,1},{1,0,1},{1,1,1},{0,1,1}},
          {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
           {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}}};
}
static double average_z(const VolumeSessionV25 &s){
  double z=0;
  if(s.backend==VolumeBackendV25::LiquidPBF){
    for(const auto&p:s.liquid)z+=p.position.z;
    return z/s.liquid.size();
  }
  for(const auto&p:s.clay)z+=p.position.z;
  return z/s.clay.size();
}
int main(){
  auto mesh=cube();
  const auto points=seed_volume_v25(mesh,.25);
  assert(points.size()==64);
  auto surface=extract_volume_v25(points,.10,.25);
  validate_closed_volume_v25(surface);
  assert(!surface.positions.empty()&&!surface.triangles.empty());
  for(auto backend:{VolumeBackendV25::LiquidPBF,VolumeBackendV25::ViscoplasticMPM}){
    const auto before=begin_volume_v25(mesh,backend,.25);
    VolumeFrameV25 frame;frame.dt=.002;frame.substeps=2;
    const auto after=stage_volume_frame_v25(before,frame);
    assert(before.time==0 && after.time==frame.dt);
    assert(average_z(after)<average_z(before));
    assert(before.liquid.size()==after.liquid.size()&&before.clay.size()==after.clay.size());
    if(backend==VolumeBackendV25::ViscoplasticMPM){
      double mass=0;
      for(const auto&p:after.clay)mass+=p.mass;
      assert(std::abs(mass-1)<1e-12);
    }
    auto result=reconstruct_volume_v25(after,.10,.25);
    validate_closed_volume_v25(result);
    std::cout<<"backend="<<int(backend)<<" z_before="<<average_z(before)
             <<" z_after="<<average_z(after)<<" vertices="<<result.positions.size()
             <<" faces="<<result.triangles.size()<<std::endl;
    bool rejected=false;frame.dt=-1;
    try{stage_volume_frame_v25(after,frame);}catch(const std::invalid_argument&){rejected=true;}
    assert(rejected&&after.time==.002);
  }
  mesh.triangles.pop_back();
  bool rejected=false;
  try{seed_volume_v25(mesh,.25);}catch(const std::invalid_argument&){rejected=true;}
  assert(rejected);
}
