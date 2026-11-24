/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Measures C++ sparse editor staging vs full-stroke baseline scanning.
 * This is NOT a Blender PBVH/CCG/Undo or CUDA benchmark.
 */
#include "physics_executor.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace sculpt_physics;
using Clock=std::chrono::steady_clock;

namespace {
double quantile(std::vector<double> samples,double f)
{
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(),samples.end());
  const std::size_t at=static_cast<std::size_t>((samples.size()-1)*f);
  return samples[at];
}
void print_times(const char *label,std::vector<double> samples)
{
  std::cout<<label<<"_us_p50="<<std::setprecision(3)<<std::fixed
           <<quantile(samples,0.5)<<" "<<label
           <<"_us_p95="<<quantile(samples,0.95)<<"\n";
}
}

int main(int argc,char **argv)
{
  const std::size_t count=argc>1 ? std::strtoull(argv[1],nullptr,10) : 50000;
  const int dabs=argc>2 ? std::atoi(argv[2]) : 50;
  if (count<100 || count>1000000 || dabs<3 || dabs>10000) {
    throw std::invalid_argument("Usage: bench_executor_writeback [100..1M] [3..10000 dabs]");
  }
  World world;
  std::vector<std::size_t> all;
  all.reserve(count);
  for (std::size_t i=0;i<count;++i) {
    world.add_particle({static_cast<double>(i),0,0});
    all.push_back(i);
  }
  const auto island=build_gpu_local_island(world,all,{},all,41,false);
  auto exec=make_cpu_xpbd_executor();
  exec->allocate_island(island);
  exec->begin_stroke();

  const std::vector<std::size_t> ids{2,count/4,count/2,count-2};
  std::vector<double> sparse_us,ack_us,fullscan_us;
  sparse_us.reserve(dabs);
  ack_us.reserve(dabs);
  fullscan_us.reserve(dabs);
  for (int dab=0;dab<dabs;++dab) {
    std::vector<ExecutorEdit> edits;
    edits.reserve(ids.size());
    for (std::size_t id:ids) {
      edits.push_back({id,{static_cast<double>(id),0.01*(dab+1),0}});
    }
    exec->upload_dirty(edits);
    const auto a=Clock::now();
    const ExecutorWriteBatch staged=exec->stage_incremental();
    const auto b=Clock::now();
    if (staged.writes.size()!=ids.size()) {
      throw std::runtime_error("Incremental writeback lost dirty vertices");
    }
    exec->acknowledge_incremental(staged.revision);
    const auto c=Clock::now();
    const auto full=exec->download_dirty();
    const auto d=Clock::now();
    if (full.size()!=ids.size()) throw std::runtime_error("Full-stroke baseline lost changes");
    if (dab>0) { // first dab includes one-time cache/lazy allocations
      sparse_us.push_back(std::chrono::duration<double,std::micro>(b-a).count());
      ack_us.push_back(std::chrono::duration<double,std::micro>(c-b).count());
      fullscan_us.push_back(std::chrono::duration<double,std::micro>(d-c).count());
    }
  }
  exec->cancel();
  std::cout<<"C++ proxy-only writeback; no Blender/CUDA. vertices="<<count
           <<" changed_per_dab="<<ids.size()<<" dabs="<<dabs<<"\n";
  print_times("sparse_stage",sparse_us);
  print_times("sparse_ack",ack_us);
  print_times("full_stroke_scan",fullscan_us);
  return 0;
}
