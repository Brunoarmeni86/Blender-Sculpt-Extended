/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d15_attribute_transfer.hh"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace sculpt_physics::attribute_transfer_d15;
static void check(bool b,const char* s){if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::abort();}}
static bool near(double a,double b){return std::abs(a-b)<1e-12;}
int main(){
  Triangle t;t.id=10;t.face=3;t.component=2;t.face_set=7;t.material=4;t.mask={0.,.5,1.};
  t.uv={{1,20,{{{0,0},{1,0},{0,1}}}},{2,30,{{{2,0},{3,0},{2,1}}}}};
  t.point_color={{3,{{{2,0,0,1},{0,2,0,1},{0,0,2,1}}}}};
  t.corner_color={{4,{{{1,0,0,1},{0,1,0,1},{0,0,1,1}}}}};
  Triangle other=t;other.id=11;other.face=4;other.uv[1].chart=31;other.uv[1].values[0][0]=5.;
  Snapshot src{9,true,{other,t}};
  Request point{Domain::Point,5,9,10,2,{.5,.25,.25},{}};
  Request corner{Domain::Corner,5,9,10,2,{.5,.25,.25},{{1,20},{2,30}}};
  Request face{Domain::Face,5,9,10,2,{1,0,0},{}};
  Request wedge=corner;wedge.destination=6;wedge.triangle=11;wedge.charts[1].second=31;
  Batch result;
  check(stage(src,{face,wedge,corner,point},result)==Status::Ok,"multi-domain batch");
  check(result.records.size()==4&&result.generation==9,"batch identity");
  const auto& p=result.records[0];const auto& c=result.records[1];const auto& w=result.records[2];const auto& f=result.records[3];
  check(p.domain==Domain::Point&&p.has_mask&&near(p.mask,.375)&&near(p.colors[0].second[0],1.),"point mask and linear HDR color");
  check(c.domain==Domain::Corner&&c.uv.size()==2&&near(c.uv[0].second[0],.25)&&near(c.uv[1].second[0],2.25),"all UV layers and UDIM");
  check(near(c.colors[0].second[0],.5)&&near(w.uv[1].second[0],3.75),"corner color and distinct wedge");
  check(f.domain==Domain::Face&&f.face_set==7&&f.material==4&&f.source_face==3,"discrete face copy");
  auto bad=corner;bad.charts[1].second=31;
  check(stage(src,{point,bad},result)==Status::ChartMismatch&&result.records.size()==4,"secondary chart failure atomic");
  bad=corner;bad.generation=8;check(stage(src,{bad},result)==Status::Stale,"stale anchor");
  bad=corner;bad.component=99;check(stage(src,{bad},result)==Status::MissingSource,"component mismatch");
  bad=corner;bad.triangle=99;check(stage(src,{bad},result)==Status::MissingSource,"no fabricated source for new surface");
  bad=corner;bad.bary={-.1,.5,.6};check(stage(src,{bad},result)==Status::Invalid,"invalid barycentric weights");
  check(stage(src,{corner,corner},result)==Status::DuplicateDestination,"duplicate destination");
  auto invalid=src;invalid.triangles[0].uv[1].layer=9;check(stage(invalid,{point},result)==Status::Invalid,"schema mismatch");
  invalid=src;invalid.triangles[0].point_color[0].values[0][0]=std::numeric_limits<double>::quiet_NaN();
  check(stage(invalid,{point},result)==Status::Invalid,"nonfinite source");
  invalid=src;invalid.triangles[0].id=10;check(stage(invalid,{point},result)==Status::Invalid,"duplicate source");
  check(result.records.size()==4&&result.generation==9&&near(result.records[1].uv[1].second[0],2.25),"failed batches leave prior output intact");
  std::reverse(src.triangles.begin(),src.triangles.end());
  check(stage(src,{point,corner,wedge,face},result)==Status::Ok&&near(result.records[2].uv[1].second[0],3.75),"order independent");
  src.has_mask=false;check(stage(src,{point},result)==Status::Ok&&!result.records[0].has_mask,"absent mask not fabricated");
  check(stage(src,{},result)==Status::Ok&&result.records.empty(),"empty batch");
  std::puts("D15_ATTRIBUTE_TRANSFER_REFERENCE_OK");
}
