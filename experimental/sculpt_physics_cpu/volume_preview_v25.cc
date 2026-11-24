/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "volume_session_v25.hh"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace sculpt_physics;
static VolumeSurfaceV25 read_obj(const std::string &path){
  std::ifstream in(path);
  if(!in)throw std::runtime_error("Cannot read input OBJ");
  VolumeSurfaceV25 mesh;
  std::string line;
  while(std::getline(in,line)){
    std::istringstream row(line);std::string kind;row>>kind;
    if(kind=="v"){
      Vec3 p;if(!(row>>p.x>>p.y>>p.z))throw std::invalid_argument("Invalid OBJ position");
      mesh.positions.push_back(p);
    }else if(kind=="f"){
      VolumeTriangleV25 t{};std::string token;
      for(int i=0;i<3;++i){
        if(!(row>>token))throw std::invalid_argument("OBJ must be triangulated");
        auto index=token.substr(0,token.find('/'));std::size_t end=0;
        const auto v=std::stoll(index,&end);
        if(end!=index.size()||v<=0||std::size_t(v)>mesh.positions.size())
          throw std::invalid_argument("OBJ requires positive existing vertex indices");
        t[i]=std::size_t(v-1);
      }
      if(row>>token && token[0]!='#')throw std::invalid_argument("OBJ must be triangulated");
      mesh.triangles.push_back(t);
    }
    if(mesh.positions.size()>100000||mesh.triangles.size()>200000)
      throw std::length_error("Preview input mesh budget exceeded");
  }
  if(in.bad())throw std::runtime_error("OBJ read failed");
  return mesh;
}
static void write_obj(const VolumeSurfaceV25 &mesh,const std::string &path){
  if(std::filesystem::exists(path))throw std::runtime_error("Output already exists");
  const auto temp=path+".tmp";
  if(std::filesystem::exists(temp))throw std::runtime_error("Temporary output already exists");
  try{
    std::ofstream out(temp);
    if(!out)throw std::runtime_error("Cannot write preview OBJ");
    out<<"# BSE V25 volumetric preview; new topology, no transferred attributes\n";
    out<<std::setprecision(17);
    for(const auto&p:mesh.positions)out<<"v "<<p.x<<" "<<p.y<<" "<<p.z<<"\n";
    for(const auto&t:mesh.triangles)out<<"f "<<t[0]+1<<" "<<t[1]+1<<" "<<t[2]+1<<"\n";
    out.close();if(!out)throw std::runtime_error("Preview OBJ write failed");
    std::filesystem::rename(temp,path);
  }catch(...){std::filesystem::remove(temp);throw;}
}
int main(int argc,char **argv){
  if(argc!=9&&argc!=10){
    std::cerr<<"Usage: volume_preview_v25 input.obj output.obj pbf|mpm spacing frames frame_dt softening gravity_z [floor_z]\n";
    return 2;
  }
  try{
    const std::string mode=argv[3];
    if(mode!="pbf"&&mode!="mpm")throw std::invalid_argument("Backend must be pbf or mpm");
    const double dx=std::stod(argv[4]);const int frames=std::stoi(argv[5]);
    if(frames<0||frames>100)throw std::invalid_argument("Frame count must be 0 to 100");
    VolumeFrameV25 f;f.dt=std::stod(argv[6]);f.softening=std::stod(argv[7]);
    f.gravity={0,0,std::stod(argv[8])};f.substeps=4;
    if(argc==10){f.floor_enabled=true;f.floor_z=std::stod(argv[9]);}
    const auto mesh=read_obj(argv[1]);
    auto s=begin_volume_v25(mesh,mode=="pbf"?VolumeBackendV25::LiquidPBF:
                           VolumeBackendV25::ViscoplasticMPM,dx);
    std::cout<<"particles="<<s.liquid.size()+s.clay.size()<<std::endl;
    for(int i=0;i<frames;++i){
      s=stage_volume_frame_v25(s,f);
      std::cout<<"frame="<<i+1<<" time="<<s.time<<std::endl;
    }
    auto result=reconstruct_volume_v25(s,dx*.5,dx*.95);
    write_obj(result,argv[2]);
    std::cout<<"vertices="<<result.positions.size()<<" triangles="<<result.triangles.size()<<std::endl;
  }catch(const std::exception &e){std::cerr<<"[V25 volume] "<<e.what()<<std::endl;return 1;}
}
