/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d13_smooth_recovery.hh"
#include <cstdlib>
#include <cstdio>
using namespace sculpt_physics::smooth_recovery_d13;
static void check(bool b){if(!b)std::abort();}
int main(){
 check(parse(nullptr)==Policy::Adaptive);
 check(parse("typo")==Policy::Invalid);
 for(auto n:{std::size_t(75000),std::size_t(75001),std::size_t(200001),std::size_t(500001)}){
   auto c=plan(Policy::Classic,100.,n),o=plan(Policy::Original,100.,n),
        e=plan(Policy::Extended,100.,n),a=plan(Policy::Adaptive,100.,n);
   check(c.kernel_mode==0&&c.executed==24);
   check(o.kernel_mode==4&&o.executed==12);
   check(e.kernel_mode==4&&e.executed==24);
   check(a.executed==(n>500000?4:n>200000?6:n>75000?8:12));
 }
 check(plan(Policy::Original,10.,1).executed==3);
 check(plan(Policy::Classic,10.,1).executed==4);
 check(plan(Policy::Classic,1e300,1).executed==24);
 check(plan(Policy::Invalid,10.,1).executed==0);
 std::puts("D13_SMOOTH_PLAN_OK");
}
