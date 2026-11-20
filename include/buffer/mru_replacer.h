#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "buffer/frame.h"
#include "common/fixed_array.h"

namespace mvcc {

// 最近使用的优先淘汰。装入与命中都进表头，选牺牲帧从表头开始找 pin = 0。
// 扫描装入的页天然落在表头，符合"扫描装入优先淘汰"，不需要额外标记。
class MruReplacer {
 public:
  explicit MruReplacer(size_t frame_count);

  void OnHit(size_t frame, PageId page, AccessMode mode);
  void OnLoad(size_t frame, PageId page, AccessMode mode);
  void OnEvict(size_t frame, PageId page);

  std::optional<size_t> SelectVictim(PageId, std::span<const Frame> frames) const;
  bool HasEvictable(std::span<const Frame> frames) const;

 private:
  void PushFront(size_t frame);
  void Remove(size_t frame);

  FixedArray<size_t> prev_;
  FixedArray<size_t> next_;
  FixedArray<bool> linked_;
  size_t head_ = kNoFrame;
};

}  // namespace mvcc
