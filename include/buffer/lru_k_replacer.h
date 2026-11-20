#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "buffer/frame.h"
#include "common/fixed_array.h"

namespace mvcc {

// LRU-2：按倒数第二次访问的时间排淘汰顺序。只被访问过一次的页（含扫描装入）
// 归入最优先组，组内按最近一次访问时间排；访问过两次以上的按倒数第二次的
// 时间排。扫描装入的页 last_ 取 0，落在组内最前。
class LruKReplacer {
 public:
  explicit LruKReplacer(size_t frame_count);

  void OnHit(size_t frame, PageId page, AccessMode mode);
  void OnLoad(size_t frame, PageId page, AccessMode mode);
  void OnEvict(size_t frame, PageId page);
  void OnMiss(PageId) {}

  std::optional<size_t> SelectVictim(PageId, std::span<const Frame> frames) const;
  bool HasEvictable(std::span<const Frame> frames) const;

 private:
  FixedArray<uint64_t> last_;
  FixedArray<uint64_t> second_last_;
  FixedArray<bool> resident_;
  uint64_t tick_ = 0;
};

}  // namespace mvcc
