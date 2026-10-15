/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
namespace blender::draw::multires_draw_profile_v01 {
inline const char *path()
{
  const char *value = std::getenv("BLENDER_MULTIRES_DRAW_PROFILE_LOG");
  return value && value[0] ? value : nullptr;
}
inline void log(const char *format, ...)
{
  const char *filename = path();
  if (!filename) { return; }
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  FILE *file = std::fopen(filename, "a");
  if (!file) { return; }
  va_list args;
  va_start(args, format);
  std::vfprintf(file, format, args);
  va_end(args);
  std::fclose(file);
}
}
