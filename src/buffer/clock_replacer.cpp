#include "buffer/clock_replacer.h"

#include <atomic>
#include <cassert>

namespace mvcc {

ClockReplacer::ClockReplacer(size_t frame_count) : states_(frame_count) {
  assert(frame_count > 0);
  for (size_t frame = 0; frame < frame_count; ++frame) states_[frame] = State::kAbsent;
}

void ClockReplacer::OnHit(size_t frame, PageId, AccessMode mode) {
  if (mode != AccessMode::kNormal) return;
  if (states_[frame] == State::kAbsent) return;
  states_[frame] = State::kHot;
}

void ClockReplacer::OnLoad(size_t frame, PageId, AccessMode mode) {
  states_[frame] = mode == AccessMode::kScan ? State::kCold : State::kHot;
}

void ClockReplacer::OnEvict(size_t frame, PageId) { states_[frame] = State::kAbsent; }

std::optional<size_t> ClockReplacer::SelectVictim(PageId, std::span<const Frame> frames) const {
  const size_t count = states_.size();
  for (size_t scanned = 0; scanned < count; ++scanned) {
    const size_t frame = hand_;
    hand_ = (hand_ + 1) % count;

    if (states_[frame] == State::kAbsent) continue;
    if (!Evictable(frames[frame])) continue;
    if (states_[frame] == State::kHot) {
      states_[frame] = State::kCold;
      continue;
    }
    return frame;
  }
  return std::nullopt;
}

// 纯查询：只回答"有没有可淘汰的帧"，不推进 hand、不降冷。
// 热的驻留帧也算可淘汰——SelectVictim 第一圈会把它降冷，第二圈就能选中。
bool ClockReplacer::HasEvictable(std::span<const Frame> frames) const {
  for (size_t frame = 0; frame < states_.size(); ++frame) {
    if (states_[frame] == State::kAbsent) continue;
    if (Evictable(frames[frame])) return true;
  }
  return false;
}

}  // namespace mvcc
