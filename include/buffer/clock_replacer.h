#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "buffer/frame.h"
#include "common/fixed_array.h"

namespace mvcc {

// 二次机会：每个驻留帧一个引用位，淘汰时从 hand 起扫一圈，热的降冷、冷的
// 直接选中。扫描装入的页置冷（优先淘汰），扫描命中不置位（已驻留页不因
// 扫描刷新，§7.2）。
class ClockReplacer {
 public:
  explicit ClockReplacer(size_t frame_count);

  void OnHit(size_t frame, PageId page, AccessMode mode);
  void OnLoad(size_t frame, PageId page, AccessMode mode);
  void OnEvict(size_t frame, PageId page);

  std::optional<size_t> SelectVictim(PageId, std::span<const Frame> frames) const;
  bool HasEvictable(std::span<const Frame> frames) const;

 private:
  enum class State : uint8_t { kAbsent, kCold, kHot };

  // SelectVictim 虽是 const，却会推进 hand 并把热帧降冷——"给第二次机会"本身
  // 就是选帧过程的一部分。因此它不能用作条件变量的谓词（改完状态返回 false
  // 就丢掉这次唤醒），等待方一律用 HasEvictable。
  mutable FixedArray<State> states_;
  mutable size_t hand_ = 0;
};

}  // namespace mvcc
