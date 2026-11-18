#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "buffer/buffer_pool.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {

// 扫描的副产品：行数、走链行数、总跳数、可见数据的折叠值，以及本次扫描期间
// 两个池的计数增量（对应结果 JSON 里 details[] 的每扫描字段）。
struct ScanResult {
  size_t rows = 0;
  size_t chain_rows = 0;
  size_t chain_hops = 0;
  uint64_t checksum = 0;
  PoolStats base_stats;
  PoolStats undo_stats;
};

namespace scan_detail {

// 四个累加器打散依赖链：逐字串行 XOR 会卡在每字一拍的延迟上。
template <typename LoadWord>
inline uint64_t FoldWords(size_t word_count, LoadWord load) {
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t c = 0;
  uint64_t d = 0;
  size_t index = 0;

  for (; index + 4 <= word_count; index += 4) {
    a ^= load(index);
    b ^= load(index + 1);
    c ^= load(index + 2);
    d ^= load(index + 3);
  }
  for (; index < word_count; ++index) a ^= load(index);

  return a ^ std::rotl(b, 17) ^ std::rotl(c, 31) ^ std::rotl(d, 47);
}

inline PoolStats StatsDelta(const PoolStats& after, const PoolStats& before) {
  return PoolStats{after.logical_reads - before.logical_reads,
                   after.logical_misses - before.logical_misses,
                   after.physical_reads - before.physical_reads,
                   after.physical_read_ops - before.physical_read_ops,
                   after.physical_writes - before.physical_writes};
}

}  // namespace scan_detail

// 折叠一段字节。只用于跨实现比对：XOR 可交换，所以整表的期望值可以按行
// 独立算出再合并，不依赖扫描顺序，也不受补齐行影响。
inline uint64_t ChecksumBytes(std::span<const std::byte> bytes) {
  return scan_detail::FoldWords(bytes.size() / sizeof(uint64_t), [bytes](size_t index) {
    uint64_t word = 0;
    std::memcpy(&word, bytes.data() + index * sizeof(uint64_t), sizeof(word));
    return word;
  });
}

// 全表扫描的基线策略：按页号升序逐页扫，需要旧版本时立即沿 undo 链回溯。
// 页按批预取，一次 Fix 覆盖整页的行——逐行 Fix 会给每行再加一次池锁往返，
// 把行级成本埋掉。
template <typename BasePool, typename UndoPool>
class TableScanner {
 public:
  TableScanner(BasePool& base_pool, UndoPool& undo_pool, const TableLayout& layout)
      : base_pool_(&base_pool), undo_pool_(&undo_pool), layout_(&layout) {}

  TableScanner(const TableScanner&) = delete;
  TableScanner& operator=(const TableScanner&) = delete;

  ScanResult ScanBaseline(Timestamp read_ts) {
    const PoolStats base_before = base_pool_->stats();
    const PoolStats undo_before = undo_pool_->stats();

    ScanResult result;
    const size_t page_count = layout_->page_count();
    const size_t capacity = base_pool_->prefetch_capacity();
    const size_t batch = capacity == 0 ? 1 : capacity;

    for (size_t first = 0; first < page_count; first += batch) {
      const size_t count = std::min(batch, page_count - first);
      if (capacity != 0) {
        base_pool_->Prefetch(static_cast<PageId>(first), count, AccessMode::kScan);
      }

      for (size_t index = 0; index < count; ++index) {
        Frame* frame = base_pool_->Fix(static_cast<PageId>(first + index), AccessMode::kScan);
        const Page page(frame->data, *layout_);
        for (size_t slot = 0; slot < page.row_count(); ++slot) {
          ScanRow(page.RowAt(slot), read_ts, result);
        }
        base_pool_->Unpin(frame);
      }
    }

    result.base_stats = scan_detail::StatsDelta(base_pool_->stats(), base_before);
    result.undo_stats = scan_detail::StatsDelta(undo_pool_->stats(), undo_before);
    return result;
  }

 private:
  // §9.2：数据先读，版本戳最后读；判定成立后重读链头再回溯。折叠直接在行上
  // 做，不先拷进缓冲——拷一份和折叠一样是"在版本戳之前读完数据"，但少一次
  // 每行上百字节的内存拷贝。这次折叠可能是写者改到一半的中间态，判定失败时
  // 整个 fold 被丢弃，不会计入 checksum（I7）。
  void ScanRow(Row row, Timestamp read_ts, ScanResult& result) {
    const uint64_t fold = ChecksumBytes(row.data());
    UndoRowId cursor = row.undo_row_id();
    // 数据读完之后、版本戳之前：这道 acquire 栅栏挡住"数据读被重排到版本戳
    // 之后"，否则可能拿旧凭证配新数据（I1 的顺序不能只靠版本戳的 acquire）。
    std::atomic_thread_fence(std::memory_order_acquire);
    const VersionStamp stamp = row.version();
    ++result.rows;

    if (!stamp.locked() && stamp.timestamp() < read_ts) {
      result.checksum ^= fold;
      return;
    }

    cursor = row.undo_row_id();
    ++result.chain_rows;

    for (;;) {
      assert(!cursor.is_null());
      Frame* frame = undo_pool_->Fix(cursor.page(), AccessMode::kNormal);
      const Row record = Page(frame->data, *layout_).RowAt(cursor.slot());
      ++result.chain_hops;

      if (record.version().timestamp() < read_ts) {
        result.checksum ^= ChecksumBytes(record.data());
        undo_pool_->Unpin(frame);
        return;
      }

      cursor = record.undo_row_id();
      undo_pool_->Unpin(frame);
    }
  }

  BasePool* base_pool_;
  UndoPool* undo_pool_;
  const TableLayout* layout_;
};

}  // namespace mvcc
