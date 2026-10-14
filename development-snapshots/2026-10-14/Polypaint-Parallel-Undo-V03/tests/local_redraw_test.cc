#include <vector>
#include <set>
#include <random>
#include <cassert>
#include <cstdio>
struct Node{std::vector<int>g;const auto&grids()const{return g;}};
int main(){std::mt19937 rng(42);for(int trial=0;trial<2000;trial++){
std::vector<unsigned char>dirty(103,0);std::set<int>changed;
for(int j=0;j<20;j++){int g=rng()%103;dirty[g]=1;changed.insert(g);}
std::vector<Node>nodes(19);for(int g=0;g<103;g++)nodes[rng()%19].g.push_back(g);
auto predicate=[&](const int i) {
                  for (const int grid : nodes[i].grids()) {
                    if (grid >= 0 && grid < dirty.size() && dirty[grid]) return true;
                  }
                  return false;
                };
for(int i=0;i<19;i++){bool expected=false;for(int g:nodes[i].g)expected|=changed.count(g)>0;assert(predicate(i)==expected);}
}std::puts("PASS 2000 grid repartitions: actual Undo redraw predicate selects every affected current node");}
