/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sculpt_physics::uv_journal_d15 {
using UV=std::array<float,2>;
struct Identity { std::uint64_t object=0,topology=0,corners=0; };
inline bool same(Identity a,Identity b){return a.object==b.object&&a.topology==b.topology&&a.corners==b.corners;}
using Key=std::pair<std::string,std::uint64_t>;
struct Value {Key key;UV uv;};
inline bool valid(Identity id,const Value& v){return !v.key.first.empty()&&v.key.second<id.corners&&std::isfinite(v.uv[0])&&std::isfinite(v.uv[1]);}
class Journal {
 Identity identity_;
 std::map<Key,UV> saved_;
public:
 explicit Journal(Identity id):identity_(id){}
 std::size_t size()const{return saved_.size();}
 // Preserve the first old value across repeated dabs. The temporary map
 // gives a strong exception guarantee; optimize allocation only after parity.
 bool capture(Identity id,const std::vector<Value>& before){
  if(!same(id,identity_))return false;
  std::map<Key,UV> incoming;
  for(const auto& v:before){
   if(!valid(id,v))return false;
   const auto r=incoming.emplace(v.key,v.uv);
   if(!r.second&&r.first->second!=v.uv)return false;
  }
  auto trial=saved_;
  trial.insert(incoming.begin(),incoming.end());
  saved_.swap(trial);return true;
 }
 std::vector<Value> snapshot()const{
  std::vector<Value> r;r.reserve(saved_.size());
  for(const auto& entry:saved_)r.push_back({entry.first,entry.second});
  return r;
 }
 // Caller supplies the complete current set of journal keys. Validate every
 // entry before any swap; no allocation after the preflight succeeds.
 bool exchange(Identity id,std::vector<Value>& live){
  if(!same(id,identity_)||live.size()!=saved_.size())return false;
  std::map<Key,std::size_t> index;
  for(std::size_t i=0;i<live.size();++i){
   if(!valid(id,live[i])||saved_.find(live[i].key)==saved_.end()||!index.emplace(live[i].key,i).second)return false;
  }
  for(auto& entry:saved_)std::swap(entry.second,live[index.at(entry.first)].uv);
  return true;
 }
};
} // namespace sculpt_physics::uv_journal_d15
