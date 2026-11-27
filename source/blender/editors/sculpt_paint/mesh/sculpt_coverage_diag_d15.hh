/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

namespace blender::ed::sculpt_paint::coverage_diag_d15 {
// Diagnostic only; read a bounded, comma-separated list of nonnegative IDs.
// Invalid input disables the watch rather than accidentally selecting all.
inline std::vector<int> watched_vertices()
{
  const char *p=std::getenv("BLENDER_SCULPT_D15_COVERAGE_IDS");
  if(!p||!*p)return {};
  const std::size_t n=std::strlen(p);
  if(n>65536)return {};
  const char *end=p+n;
  std::vector<int> ids;
  while(p<end){
    int id=-1;
    const auto r=std::from_chars(p,end,id);
    if(r.ec!=std::errc{}||id<0||ids.size()>=4096)return {};
    ids.push_back(id);
    p=r.ptr;
    if(p==end)break;
    if(*p!=','||++p==end)return {};
  }
  std::sort(ids.begin(),ids.end());
  ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
  return ids;
}
} // namespace blender::ed::sculpt_paint::coverage_diag_d15
