#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "buffer/frame.h"
#include "common/fixed_array.h"

namespace mvcc {

// 命中且为普通访问时移到表头；扫描访问不改动新近性。
// 装入时普通模式进表头、扫描模式进表尾（优先淘汰）。
// 选牺牲帧时从表尾向前跳过已 pin 的帧，因此不需要解固通知。
class LruReplacer {
 public:
  explicit LruReplacer(size_t frame_count);

  void OnHit(size_t frame, PageId page, AccessMode mode);
  void OnLoad(size_t frame, PageId page, AccessMode mode);
  void OnEvict(size_t frame, PageId page);

  std::optional<size_t> SelectVictim(PageId, std::span<const Frame> frames) const;
  bool HasEvictable(std::span<const Frame> frames) const;

 private:
  void PushFront(size_t frame);
  void PushBack(size_t frame);
  void Remove(size_t frame);

  FixedArray<size_t> prev_;
  FixedArray<size_t> next_;
  FixedArray<bool> linked_;
  size_t head_ = kNoFrame;
  size_t tail_ = kNoFrame;
};

}  // namespace mvcc
