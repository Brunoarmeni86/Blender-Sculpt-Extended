/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <atomic>
#include <cstdlib>

namespace blender::bke::multires_layers::undo_cache {

/* Memfile decode owns persistent mesh/modifier restoration. The old evaluated
 * CCG must not publish its composed layer surface into the native mesh while
 * that restoration is in progress. Depsgraph cleanup may use worker threads,
 * so this scope is process-wide rather than thread-local. */
inline std::atomic<unsigned int> discard_depth{0};

inline bool discard_active()
{
  return discard_depth.load(std::memory_order_acquire) != 0;
}

class ScopedDiscardCacheWrites {
  bool enabled_ = false;

 public:
  ScopedDiscardCacheWrites()
  {
    const char *value = std::getenv("BLENDER_SCULPT_LAYER_UNDO_CACHE_GUARD");
    enabled_ = !value || value[0] != '0';
    if (enabled_) {
      discard_depth.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  ~ScopedDiscardCacheWrites()
  {
    if (enabled_) {
      discard_depth.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  ScopedDiscardCacheWrites(const ScopedDiscardCacheWrites &) = delete;
  ScopedDiscardCacheWrites &operator=(const ScopedDiscardCacheWrites &) = delete;
};

}  // namespace blender::bke::multires_layers::undo_cache
