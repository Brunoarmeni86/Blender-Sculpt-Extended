/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "d15_surface_anchor.hh"
#include <cstdio>
#include <cstdlib>
using namespace sculpt_physics::surface_anchor_d15;
static void check(bool b,const char* s){if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::abort();}}
static bool near(double a,double b){return std::abs(a-b)<1e-10;}
int main(){
 Triangle a{10,1,2,{0,1,2},{{{0,0,0},{1,0,0},{0,1,0}}},{{{0,0},{1,0},{0,1}}}};
 Triangle b{11,1,2,{1,3,2},{{{1,0,0},{1,1,0},{0,1,0}}},{{{1,0},{1,1},{0,1}}}};
 Anchor seed{7,10,1,2,{.5,.25,.25}},out=seed;Sample s;
 check(evaluate(a,7,seed,s)&&near(s.uv[0],.25)&&near(s.position[1],.25),"anchor evaluate");
 check(!evaluate(a,8,seed,s)&&near(s.uv[0],.25),"generation reject");
 Query q{{.8,.8,.01},{0,0,1},.05,.5,1e-8,1e-6,2};
 std::vector<Triangle> p{a,b};std::vector<Link> l{{0,1}};
 check(relocate(p,l,7,seed,q,out)==Status::Ok&&out.triangle==11,"walk across shared edge");
 check(evaluate(b,7,out,s)&&near(s.uv[0],.8)&&near(s.uv[1],.8),"barycentric UV");
 q.position={.5,.5,0};check(relocate(p,l,7,seed,q,out)==Status::Ok&&out.triangle==10,"equivalent shared-edge tie");
 q.position={.8,.8,.01};p[1].chart=3;check(relocate(p,l,7,seed,q,out)==Status::NoMatch&&out.triangle==10,"chart barrier");
 p[1]=b;p[1].uv[0][0]+=1.;check(relocate(p,l,7,seed,q,out)==Status::NoMatch,"actual UV seam barrier");
 p[1]=b;p[1].component=3;check(relocate(p,l,7,seed,q,out)==Status::NoMatch,"component barrier");
 p[1]=b;q.max_hops=0;check(relocate(p,l,7,seed,q,out)==Status::NoMatch,"hop budget");q.max_hops=2;
 q.normal={0,0,-1};check(relocate(p,l,7,seed,q,out)==Status::NoMatch,"opposite normal");q.normal={0,0,1};
 q.position={.25,.25,.01};Triangle sheet=a;sheet.id=12;sheet.vertices={4,5,6};for(auto& x:sheet.co)x[2]=.009;
 p.push_back(sheet);l.push_back({0,2});check(relocate(p,l,7,seed,q,out)==Status::Ok&&out.triangle==10,"closer disconnected sheet excluded");
 p.pop_back();l.pop_back();
 // Two overlapping triangles share a legitimate UV-continuous edge but have
 // different interior UVs. Equal geometric hit must be rejected.
 Triangle fold{13,1,2,{0,1,4},a.co,{{{0,0},{1,0},{0,2}}}};
 p={a,fold};l={{0,1}};q.position={.25,.25,0};
 check(relocate(p,l,7,seed,q,out)==Status::Ambiguous&&out.triangle==10,"ambiguous overlap no write");
 p={a,b};q.position={2,0,0};q.max_distance=2.;V w;double d;
 check(closest(a,q.position,w,d)&&near(w[1],1.)&&near(d,1.),"edge/vertex clamp");
 p[1].co[2]=p[1].co[1];check(relocate(p,l,7,seed,q,out)==Status::Invalid,"degenerate triangle");
 check(relocate({a,a},l,7,seed,q,out)==Status::Invalid,"duplicate IDs");
 check(relocate({a,b},{{0,8}},7,seed,q,out)==Status::Invalid,"bad link");
 check(relocate({a,b},l,8,seed,q,out)==Status::Invalid,"stale anchor");
 q.position[0]=std::numeric_limits<double>::quiet_NaN();check(relocate({a,b},l,7,seed,q,out)==Status::Invalid,"NaN query");
 std::puts("D15_SURFACE_ANCHOR_REFERENCE_OK");
}
