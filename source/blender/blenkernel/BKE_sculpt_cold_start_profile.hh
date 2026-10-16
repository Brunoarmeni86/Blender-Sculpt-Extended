/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "BLI_time.h"

/* Diagnostic only. No retained Object/CCG/PBVH pointers and no sample traversal. */
namespace blender::bke::sculpt_cold_v01 {
inline const char *path()
{
  const char *p = std::getenv("BLENDER_SCULPT_COLD_START_LOG");
  return p && p[0] ? p : nullptr;
}
inline std::mutex &log_mutex() { static std::mutex value; return value; }
inline void event(const char *kind, const char *format, ...)
{
  const char *p = path();
  if (!p) return;
  const double now = BLI_time_now_seconds();
  std::lock_guard<std::mutex> lock(log_mutex());
  FILE *file = std::fopen(p, "a");
  if (!file) return;
  std::fprintf(file, "t_s=%.6f event=%s ", now, kind);
  va_list args;
  va_start(args, format);
  std::vfprintf(file, format, args);
  va_end(args);
  std::fputc('\n', file);
  std::fclose(file);
}
class Scope {
  const char *name_;
  const void *identity_;
  bool enabled_;
  double start_ = 0.0, last_ = 0.0;
 public:
  explicit Scope(const char *name, const void *identity = nullptr)
      : name_(name), identity_(identity), enabled_(path() != nullptr)
  {
    if (enabled_) {
      start_ = last_ = BLI_time_now_seconds();
      event("BEGIN", "scope=%s id=%p object=%p", name_,
            static_cast<const void *>(this), identity_);
    }
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  void mark(const char *stage)
  {
    if (!enabled_) return;
    const double now = BLI_time_now_seconds();
    event("STAGE", "scope=%s id=%p stage=%s dt_ms=%.3f total_ms=%.3f", name_,
          static_cast<const void *>(this), stage, (now-last_)*1000.0,
          (now-start_)*1000.0);
    last_ = BLI_time_now_seconds();
  }
  ~Scope()
  {
    if (enabled_) {
      const double elapsed = (BLI_time_now_seconds()-start_)*1000.0;
      event("END", "scope=%s id=%p object=%p total_ms=%.3f", name_,
            static_cast<const void *>(this), identity_, elapsed);
    }
  }
};
}  // namespace blender::bke::sculpt_cold_v01
