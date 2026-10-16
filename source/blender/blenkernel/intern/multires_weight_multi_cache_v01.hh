/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>
#include <list>

namespace blender::bke::multires_layers::weight_multi_v01 {

/* Callers hold the existing weight-cache mutex for every access, including
 * the parallel apply barrier. Nodes never move while worker threads read them.
 * Charges include payload, lookup storage and a conservative metadata allowance.
 * This limits cache-owned storage, not Blender's total process memory. */
template<typename Entry> class BoundedLRU {
  struct Node {
    int64_t charge;
    Entry value;
    explicit Node(const int64_t bytes) : charge(bytes) {}
  };
  std::list<Node> nodes_;
  int64_t used_ = 0;

 public:
  int64_t bytes() const { return used_; }
  int size() const { return int(nodes_.size()); }

  template<typename Predicate> int invalidate(Predicate predicate)
  {
    int removed = 0;
    for (auto it = nodes_.begin(); it != nodes_.end();) {
      if (predicate(it->value)) {
        used_ -= it->charge;
        it = nodes_.erase(it);
        removed++;
      }
      else {
        ++it;
      }
    }
    return removed;
  }

  template<typename Predicate> Entry *find(Predicate predicate)
  {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
      if (predicate(it->value)) {
        nodes_.splice(nodes_.begin(), nodes_, it);
        return &nodes_.front().value;
      }
    }
    return nullptr;
  }

  Entry *insert(const int64_t charge, const int64_t budget, int &r_evicted)
  {
    r_evicted = 0;
    /* A non-cacheable layer must not evict useful cached layers. */
    if (charge <= 0 || budget <= 0 || charge > budget) {
      return nullptr;
    }
    while (!nodes_.empty() &&
           (used_ > budget - charge || nodes_.size() >= 64))
    {
      used_ -= nodes_.back().charge;
      nodes_.pop_back();
      r_evicted++;
    }
    /* Construct before charging: a failed construction cannot publish an entry. */
    nodes_.emplace_front(charge);
    used_ += charge;
    return &nodes_.front().value;
  }
};
}  // namespace blender::bke::multires_layers::weight_multi_v01
