#include "buffer/arc_replacer.h"

#include <algorithm>
#include <atomic>
#include <cassert>

namespace mvcc {
namespace {

size_t Ratio(size_t numerator, size_t denominator) {
  return std::max<size_t>(1, numerator / std::max<size_t>(1, denominator));
}

}  // namespace

ArcReplacer::ArcReplacer(size_t frame_count, size_t page_count)
    : prev_(frame_count),
      next_(frame_count),
      tier_(frame_count),
      cold_(frame_count),
      ghost_(page_count),
      ghost_prev_(page_count),
      ghost_next_(page_count),
      capacity_(frame_count) {
  assert(frame_count > 0);
  assert(page_count > 0);

  for (size_t frame = 0; frame < frame_count; ++frame) {
    prev_[frame] = kNoFrame;
    next_[frame] = kNoFrame;
    tier_[frame] = Tier::kNone;
    cold_[frame] = false;
  }
  for (size_t page = 0; page < page_count; ++page) {
    ghost_[page] = 0;
    ghost_prev_[page] = kNoGhost;
    ghost_next_[page] = kNoGhost;
  }
}

void ArcReplacer::PushFront(Tier tier, size_t frame) {
  prev_[frame] = kNoFrame;
  if (tier == Tier::kT1) {
    next_[frame] = t1_head_;
    if (t1_head_ != kNoFrame) prev_[t1_head_] = frame;
    t1_head_ = frame;
    if (t1_tail_ == kNoFrame) t1_tail_ = frame;
    ++t1_size_;
  } else {
    next_[frame] = t2_head_;
    if (t2_head_ != kNoFrame) prev_[t2_head_] = frame;
    t2_head_ = frame;
    if (t2_tail_ == kNoFrame) t2_tail_ = frame;
    ++t2_size_;
  }
  tier_[frame] = tier;
}

void ArcReplacer::Remove(Tier tier, size_t frame) {
  if (tier == Tier::kNone || tier_[frame] != tier) return;

  if (prev_[frame] != kNoFrame) {
    next_[prev_[frame]] = next_[frame];
  } else if (tier == Tier::kT1) {
    t1_head_ = next_[frame];
  } else {
    t2_head_ = next_[frame];
  }

  if (next_[frame] != kNoFrame) {
    prev_[next_[frame]] = prev_[frame];
  } else if (tier == Tier::kT1) {
    t1_tail_ = prev_[frame];
  } else {
    t2_tail_ = prev_[frame];
  }

  prev_[frame] = kNoFrame;
  next_[frame] = kNoFrame;
  tier_[frame] = Tier::kNone;
  if (tier == Tier::kT1) {
    --t1_size_;
  } else {
    --t2_size_;
  }
}

void ArcReplacer::PushGhost(bool to_b1, PageId page) {
  if (page >= ghost_.size()) return;
  RemoveGhost(page);

  ghost_[page] = to_b1 ? 1 : 2;
  ghost_prev_[page] = kNoGhost;
  if (to_b1) {
    ghost_next_[page] = b1_head_;
    if (b1_head_ != kNoGhost) ghost_prev_[b1_head_] = page;
    b1_head_ = page;
    if (b1_tail_ == kNoGhost) b1_tail_ = page;
    ++b1_size_;
  } else {
    ghost_next_[page] = b2_head_;
    if (b2_head_ != kNoGhost) ghost_prev_[b2_head_] = page;
    b2_head_ = page;
    if (b2_tail_ == kNoGhost) b2_tail_ = page;
    ++b2_size_;
  }

  // 幽灵条目总量不超过缓存容量，超了就从较长的链尾丢最老的。
  while (b1_size_ + b2_size_ > capacity_) {
    if (b1_size_ >= b2_size_ && b1_tail_ != kNoGhost) {
      RemoveGhost(b1_tail_);
    } else if (b2_tail_ != kNoGhost) {
      RemoveGhost(b2_tail_);
    } else {
      break;
    }
  }
}

void ArcReplacer::RemoveGhost(PageId page) {
  if (page >= ghost_.size()) return;
  const uint8_t which = ghost_[page];
  if (which == 0) return;

  if (ghost_prev_[page] != kNoGhost) {
    ghost_next_[ghost_prev_[page]] = ghost_next_[page];
  } else if (which == 1) {
    b1_head_ = ghost_next_[page];
  } else {
    b2_head_ = ghost_next_[page];
  }

  if (ghost_next_[page] != kNoGhost) {
    ghost_prev_[ghost_next_[page]] = ghost_prev_[page];
  } else if (which == 1) {
    b1_tail_ = ghost_prev_[page];
  } else {
    b2_tail_ = ghost_prev_[page];
  }

  ghost_[page] = 0;
  ghost_prev_[page] = kNoGhost;
  ghost_next_[page] = kNoGhost;
  if (which == 1) {
    --b1_size_;
  } else {
    --b2_size_;
  }
}

void ArcReplacer::OnHit(size_t frame, PageId, AccessMode mode) {
  if (mode != AccessMode::kNormal) return;
  cold_[frame] = false;
  const Tier tier = tier_[frame];
  if (tier == Tier::kNone) return;
  if (tier == Tier::kT1) {
    Remove(Tier::kT1, frame);
    PushFront(Tier::kT2, frame);
  } else {
    Remove(Tier::kT2, frame);
    PushFront(Tier::kT2, frame);
  }
}

void ArcReplacer::OnLoad(size_t frame, PageId page, AccessMode mode) {
  // 幽灵命中说明这个页被淘汰之后又被访问了：装回 T2，并按命中前 B1/B2 的
  // 比例调整目标 T1 大小。比例必须在 RemoveGhost 之前算，否则分母已经变了。
  const uint8_t which = page < ghost_.size() ? ghost_[page] : 0;
  const size_t b1_before = b1_size_;
  const size_t b2_before = b2_size_;

  if (which == 1) {
    target_t1_ = std::min(capacity_, target_t1_ + Ratio(b2_before, b1_before));
    RemoveGhost(page);
  } else if (which == 2) {
    const size_t step = Ratio(b1_before, b2_before);
    target_t1_ = step < target_t1_ ? target_t1_ - step : 0;
    RemoveGhost(page);
  }

  Remove(tier_[frame], frame);
  PushFront(which == 0 ? Tier::kT1 : Tier::kT2, frame);
  cold_[frame] = mode == AccessMode::kScan;
}

void ArcReplacer::OnEvict(size_t frame, PageId page) {
  const Tier tier = tier_[frame];
  cold_[frame] = false;
  if (tier == Tier::kNone) {
    PushGhost(true, page);
    return;
  }
  Remove(tier, frame);
  PushGhost(tier == Tier::kT1, page);
}

std::optional<size_t> ArcReplacer::SelectVictim(PageId page,
                                                std::span<const Frame> frames) const {
  const auto pick = [&](size_t tail, bool cold_only) -> std::optional<size_t> {
    for (size_t frame = tail; frame != kNoFrame; frame = prev_[frame]) {
      if (!Evictable(frames[frame])) continue;
      if (cold_only && !cold_[frame]) continue;
      return frame;
    }
    return std::nullopt;
  };

  // 扫描装入的页先走：它们只被读过一次，留在池里只会挤掉热页。T1 头恰好是
  // 最近装入的位置，所以不靠位置、靠这个冷标记（§7.2）。
  if (const auto victim = pick(t1_tail_, true)) return victim;
  if (const auto victim = pick(t2_tail_, true)) return victim;

  const bool ghost_b2_hit = page < ghost_.size() && ghost_[page] == 2;
  const bool prefer_t1 = t1_size_ > target_t1_ || (t1_size_ == target_t1_ && ghost_b2_hit);

  if (prefer_t1) {
    if (const auto victim = pick(t1_tail_, false)) return victim;
    return pick(t2_tail_, false);
  }
  if (const auto victim = pick(t2_tail_, false)) return victim;
  return pick(t1_tail_, false);
}

bool ArcReplacer::HasEvictable(std::span<const Frame> frames) const {
  const auto any = [&](size_t tail) {
    for (size_t frame = tail; frame != kNoFrame; frame = prev_[frame]) {
      if (Evictable(frames[frame])) return true;
    }
    return false;
  };
  return any(t1_tail_) || any(t2_tail_);
}

}  // namespace mvcc
