#include "buffer/lru_replacer.h"

#include <atomic>

namespace mvcc {

LruReplacer::LruReplacer(size_t frame_count)
    : prev_(frame_count), next_(frame_count), linked_(frame_count) {
  for (size_t frame = 0; frame < frame_count; ++frame) {
    prev_[frame] = kNoFrame;
    next_[frame] = kNoFrame;
    linked_[frame] = false;
  }
}

void LruReplacer::PushFront(size_t frame) {
  prev_[frame] = kNoFrame;
  next_[frame] = head_;
  if (head_ != kNoFrame) prev_[head_] = frame;
  head_ = frame;
  if (tail_ == kNoFrame) tail_ = frame;
  linked_[frame] = true;
}

void LruReplacer::PushBack(size_t frame) {
  next_[frame] = kNoFrame;
  prev_[frame] = tail_;
  if (tail_ != kNoFrame) next_[tail_] = frame;
  tail_ = frame;
  if (head_ == kNoFrame) head_ = frame;
  linked_[frame] = true;
}

void LruReplacer::Remove(size_t frame) {
  if (!linked_[frame]) return;

  if (prev_[frame] != kNoFrame) {
    next_[prev_[frame]] = next_[frame];
  } else {
    head_ = next_[frame];
  }
  if (next_[frame] != kNoFrame) {
    prev_[next_[frame]] = prev_[frame];
  } else {
    tail_ = prev_[frame];
  }

  prev_[frame] = kNoFrame;
  next_[frame] = kNoFrame;
  linked_[frame] = false;
}

void LruReplacer::OnHit(size_t frame, AccessMode mode) {
  if (mode != AccessMode::kNormal || !linked_[frame]) return;
  Remove(frame);
  PushFront(frame);
}

void LruReplacer::OnLoad(size_t frame, AccessMode mode) {
  Remove(frame);
  if (mode == AccessMode::kScan) {
    PushBack(frame);
  } else {
    PushFront(frame);
  }
}

void LruReplacer::OnEvict(size_t frame) { Remove(frame); }

std::optional<size_t> LruReplacer::SelectVictim(std::span<const Frame> frames) const {
  for (size_t frame = tail_; frame != kNoFrame; frame = prev_[frame]) {
    if (frames[frame].pin.load(std::memory_order_relaxed) == 0) return frame;
  }
  return std::nullopt;
}

}  // namespace mvcc
