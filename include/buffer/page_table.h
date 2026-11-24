#pragma once

#include <atomic>
#include <cstddef>
#include <optional>

#include "buffer/frame.h"
#include "common/basic_types.h"
#include "common/fixed_array.h"

namespace mvcc {

// 页号 -> 帧下标的侵入式链哈希（§7.1）。桶数为 2 的幂且约为帧数的两倍，
// 链节点复用帧下标（chain_ 以帧号为下标），零额外分配。页号无上界、无稠密
// 假设——真实系统的页号空间远大于帧数，两个池（基表、undo）都用这一种映射。
class FrameHashTable {
 public:
  explicit FrameHashTable(size_t frame_count);

  std::optional<size_t> Find(PageId page) const;
  void Insert(PageId page, size_t frame);
  void Erase(PageId page);

  // 遍历全部有效条目，供不变量检查与热点集合快照使用。
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
