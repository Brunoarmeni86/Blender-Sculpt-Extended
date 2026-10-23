/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

namespace blender::ed::sculpt_paint {

/* Losslessly compress consecutive sample IDs in encounter order. Grid IDs need
 * not be sorted; discontinuities and repeated samples start a new run. */
struct SculptSampleRun {
  int64_t start;
  int64_t size;
};

class SculptSampleRunBuilder {
  SculptSampleRun pending_{0, 0};

 public:
  template<typename Emit> void add(const int64_t sample, Emit &&emit)
  {
    if (pending_.size && sample == pending_.start + pending_.size) {
      pending_.size++;
      return;
    }
    this->finish(emit);
    pending_ = {sample, 1};
  }

  template<typename Emit> void finish(Emit &&emit)
  {
    if (pending_.size) {
      emit(pending_);
      pending_.size = 0;
    }
  }
};

}  // namespace blender::ed::sculpt_paint
