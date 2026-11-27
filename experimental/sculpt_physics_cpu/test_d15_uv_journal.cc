/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d15_uv_journal.hh"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace sculpt_physics::uv_journal_d15;
static void check(bool b,const char* s){if(!b){std::fprintf(stderr,"FAIL %s\n",s);std::abort();}}
int main(){
 Identity id{1,2,8};Journal j(id);
 check(j.capture(id,{{{"A",3},{1,2}},{{"B",3},{3,4}}}),"initial capture");
 check(j.capture(id,{{{"A",3},{5,6}},{{"A",4},{7,8}}})&&j.size()==3,"first capture");
 check(!j.capture(id,{{{"A",7},{1,1}},{{"A",8},{0,0}}})&&j.size()==3,"invalid batch atomic");
 check(!j.capture(id,{{{"A",7},{1,1}},{{"A",7},{2,2}}})&&j.size()==3,"conflicting duplicate");
 auto live=std::vector<Value>{{{"B",3},{9,10}},{{"A",4},{11,12}},{{"A",3},{13,14}}};
 auto bad=live;bad[0].key.first="renamed";
 check(!j.exchange(id,bad)&&bad[0].uv==UV{9,10},"missing layer");
 check(!j.exchange({1,3,8},live)&&live[2].uv==UV{13,14},"stale topology");
 bad=live;bad[0].uv[0]=std::numeric_limits<float>::quiet_NaN();
 check(!j.exchange(id,bad)&&bad[2].uv==UV{13,14},"nonfinite no partial swap");
 check(j.exchange(id,live)&&live[0].uv==UV{3,4}&&live[1].uv==UV{7,8}&&live[2].uv==UV{1,2},"undo");
 check(j.exchange(id,live)&&live[0].uv==UV{9,10}&&live[1].uv==UV{11,12}&&live[2].uv==UV{13,14},"redo");
 check(j.snapshot()[0].uv==UV{1,2},"first value retained after round trip");
 std::puts("D15_UV_JOURNAL_REFERENCE_OK");
}
