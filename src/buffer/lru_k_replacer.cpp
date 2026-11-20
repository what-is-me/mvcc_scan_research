#include "buffer/lru_k_replacer.h"

#include <atomic>
#include <limits>

namespace mvcc {

LruKReplacer::LruKReplacer(size_t frame_count)
    : last_(frame_count), second_last_(frame_count), resident_(frame_count) {
  for (size_t frame = 0; frame < frame_count; ++frame) {
    last_[frame] = 0;
    second_last_[frame] = 0;
    resident_[frame] = false;
  }
}

void LruKReplacer::OnHit(size_t frame, PageId, AccessMode mode) {
  if (mode != AccessMode::kNormal || !resident_[frame]) return;
  second_last_[frame] = last_[frame];
  last_[frame] = ++tick_;
}

void LruKReplacer::OnLoad(size_t frame, PageId, AccessMode mode) {
  resident_[frame] = true;
  second_last_[frame] = 0;
  // 扫描装入的页 last_ = 0，落在"只访问过一次"组的最前面，优先淘汰。
  last_[frame] = mode == AccessMode::kScan ? 0 : ++tick_;
}

void LruKReplacer::OnEvict(size_t frame, PageId) { resident_[frame] = false; }

std::optional<size_t> LruKReplacer::SelectVictim(PageId, std::span<const Frame> frames) const {
  std::optional<size_t> best;
  bool best_young = true;
  uint64_t best_order = std::numeric_limits<uint64_t>::max();

  for (size_t frame = 0; frame < resident_.size(); ++frame) {
    if (!resident_[frame]) continue;
    if (!Evictable(frames[frame])) continue;

    const bool young = second_last_[frame] == 0;
    const uint64_t order = young ? last_[frame] : second_last_[frame];

    if (!best.has_value() || (young && !best_young) ||
        (young == best_young && order < best_order)) {
      best = frame;
      best_young = young;
      best_order = order;
    }
  }
  return best;
}

bool LruKReplacer::HasEvictable(std::span<const Frame> frames) const {
  for (size_t frame = 0; frame < resident_.size(); ++frame) {
    if (resident_[frame] && Evictable(frames[frame])) return true;
  }
  return false;
}

}  // namespace mvcc
