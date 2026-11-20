#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "buffer/frame.h"
#include "common/fixed_array.h"

namespace mvcc {

inline constexpr PageId kNoGhost = ~PageId{0};

// ARC（Adaptive Replacement Cache）：T1 存只访问过一次的页，T2 存访问两次
// 以上的页，B1/B2 是两条幽灵链表，记录从 T1/T2 淘汰出去的页号。命中幽灵时
// 按 B2/B1 的比例调整目标 T1 大小，从而在"扫描型"与"热点型"负载之间自适应。
//
// 幽灵链表按页号索引（页号空间由基表大小界定），因此构造时需要 page_count。
// 扫描访问不推进 T1→T2 的提升，与 §7.2 的"已驻留页不因扫描刷新"一致。
class ArcReplacer {
 public:
  ArcReplacer(size_t frame_count, size_t page_count);

  void OnHit(size_t frame, PageId page, AccessMode mode);
  void OnLoad(size_t frame, PageId page, AccessMode mode);
  void OnEvict(size_t frame, PageId page);

  std::optional<size_t> SelectVictim(PageId page, std::span<const Frame> frames) const;

 private:
  enum class Tier : uint8_t { kNone, kT1, kT2 };

  void PushFront(Tier tier, size_t frame);
  void Remove(Tier tier, size_t frame);
  void PushGhost(bool to_b1, PageId page);
  void RemoveGhost(PageId page);

  FixedArray<size_t> prev_;
  FixedArray<size_t> next_;
  FixedArray<Tier> tier_;
  size_t t1_head_ = kNoFrame;
  size_t t1_tail_ = kNoFrame;
  size_t t2_head_ = kNoFrame;
  size_t t2_tail_ = kNoFrame;
  size_t t1_size_ = 0;
  size_t t2_size_ = 0;

  FixedArray<uint8_t> ghost_;
  FixedArray<PageId> ghost_prev_;
  FixedArray<PageId> ghost_next_;
  PageId b1_head_ = kNoGhost;
  PageId b1_tail_ = kNoGhost;
  PageId b2_head_ = kNoGhost;
  PageId b2_tail_ = kNoGhost;
  size_t b1_size_ = 0;
  size_t b2_size_ = 0;

  size_t capacity_;
  size_t target_t1_ = 0;
};

}  // namespace mvcc
