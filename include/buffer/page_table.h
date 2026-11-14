#pragma once

#include <atomic>
#include <cstddef>
#include <optional>

#include "buffer/frame.h"
#include "common/basic_types.h"
#include "common/fixed_array.h"

namespace mvcc {

// 基表池：下标即页号，一次原子读定位。
class DirectPageTable {
 public:
  explicit DirectPageTable(size_t page_count);

  std::optional<size_t> Find(PageId page) const;
  void Insert(PageId page, size_t frame);
  void Erase(PageId page);

 private:
  FixedArray<std::atomic<size_t>> slots_;
};

// undo 池：页号空间远大于帧数，桶数为 2 的幂，链节点复用帧下标。
class FrameHashTable {
 public:
  explicit FrameHashTable(size_t frame_count);

  std::optional<size_t> Find(PageId page) const;
  void Insert(PageId page, size_t frame);
  void Erase(PageId page);

 private:
  size_t BucketOf(PageId page) const;

  FixedArray<std::atomic<size_t>> buckets_;
  FixedArray<std::atomic<size_t>> chain_;
  FixedArray<PageId> keys_;
  size_t bucket_mask_ = 0;
};

}  // namespace mvcc
