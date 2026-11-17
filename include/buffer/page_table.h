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

  // 遍历全部有效条目，供不变量检查与调试使用。
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (size_t page = 0; page < slots_.size(); ++page) {
      const size_t frame = slots_[page].load();
      if (frame != kNoFrame) fn(static_cast<PageId>(page), frame);
    }
  }

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

  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (size_t bucket = 0; bucket < buckets_.size(); ++bucket) {
      for (size_t frame = buckets_[bucket].load(); frame != kNoFrame;
           frame = chain_[frame].load()) {
        fn(keys_[frame], frame);
      }
    }
  }

 private:
  size_t BucketOf(PageId page) const;

  FixedArray<std::atomic<size_t>> buckets_;
  FixedArray<std::atomic<size_t>> chain_;
  FixedArray<PageId> keys_;
  size_t bucket_mask_ = 0;
};

}  // namespace mvcc
