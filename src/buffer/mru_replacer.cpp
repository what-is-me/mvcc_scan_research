#include "buffer/mru_replacer.h"

#include <atomic>

namespace mvcc {

MruReplacer::MruReplacer(size_t frame_count)
    : prev_(frame_count), next_(frame_count), linked_(frame_count) {
  for (size_t frame = 0; frame < frame_count; ++frame) {
    prev_[frame] = kNoFrame;
    next_[frame] = kNoFrame;
    linked_[frame] = false;
  }
}

void MruReplacer::PushFront(size_t frame) {
  prev_[frame] = kNoFrame;
  next_[frame] = head_;
  if (head_ != kNoFrame) prev_[head_] = frame;
  head_ = frame;
  linked_[frame] = true;
}

void MruReplacer::Remove(size_t frame) {
  if (!linked_[frame]) return;

  if (prev_[frame] != kNoFrame) {
    next_[prev_[frame]] = next_[frame];
  } else {
    head_ = next_[frame];
  }
  if (next_[frame] != kNoFrame) prev_[next_[frame]] = prev_[frame];

  prev_[frame] = kNoFrame;
  next_[frame] = kNoFrame;
  linked_[frame] = false;
}

void MruReplacer::OnHit(size_t frame, PageId, AccessMode mode) {
  if (mode != AccessMode::kNormal || !linked_[frame]) return;
  Remove(frame);
  PushFront(frame);
}

void MruReplacer::OnLoad(size_t frame, PageId, AccessMode) {
  Remove(frame);
  PushFront(frame);
}

void MruReplacer::OnEvict(size_t frame, PageId) { Remove(frame); }

std::optional<size_t> MruReplacer::SelectVictim(PageId, std::span<const Frame> frames) const {
  for (size_t frame = head_; frame != kNoFrame; frame = next_[frame]) {
    if (Evictable(frames[frame])) return frame;
  }
  return std::nullopt;
}

bool MruReplacer::HasEvictable(std::span<const Frame> frames) const {
  for (size_t frame = head_; frame != kNoFrame; frame = next_[frame]) {
    if (Evictable(frames[frame])) return true;
  }
  return false;
}

}  // namespace mvcc
