#include <atomic>
#include <initializer_list>
#include <cassert>
#include <cstdlib>
#include <cstdio>
int waits=0,clocks=0;std::atomic<bool>*pending=nullptr;
void BLI_task_pool_work_and_wait(void*){++waits;pending->store(true,std::memory_order_release);}
double BLI_time_now_seconds(){return ++clocks*.001;}
struct PositionUndoStorage { std::atomic<bool>compression_ready{false};void*compression_task_pool=nullptr;
  void ensure_compression_complete()
  {
    if (!compression_ready.load(std::memory_order_acquire)) {
      const char *profile = std::getenv("BLENDER_SCULPT_UNDO_COMPRESSION_PROFILE");
      const bool trace = profile && profile[0] == '1' && profile[1] == '\0';
      const double started = trace ? BLI_time_now_seconds() : 0.0;
      BLI_task_pool_work_and_wait(compression_task_pool);
      if (trace) {
        std::printf("[UNDO_COMPRESSION_WAIT] elapsed=%.3fms ready=%d\n",
                    (BLI_time_now_seconds() - started) * 1000.0,
                    int(compression_ready.load(std::memory_order_acquire)));
      }
    }
  }
};
int main(){int cases=0;for(const char*profile:{"","0","1","10"})for(bool ready:{false,true}){
 setenv("BLENDER_SCULPT_UNDO_COMPRESSION_PROFILE",profile,1);waits=clocks=0;
 PositionUndoStorage storage;storage.compression_ready.store(ready);pending=&storage.compression_ready;
 storage.ensure_compression_complete();assert(waits==int(!ready));assert(storage.compression_ready.load());
 assert(clocks==((!ready&&profile[0]=='1'&&profile[1]=='\0')?2:0));++cases;
 }std::printf("PASS %d actual Undo wait/profile cases: same completion barrier; no history modifications\n",cases);}
