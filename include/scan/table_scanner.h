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
#include "buffer/pin_guard.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {

// 扫描的副产品：行数、走链行数、总跳数、可见数据的折叠值，以及本次扫描**自己**
// 发出的页请求计数（对应结果 JSON 里 details[] 的每扫描字段）。计数由池在调用
// 线程内累加到这两个字段上，不含并发 TP 的流量——池级计数器是所有线程共享的，
// 前后取差会把别人的读写算到扫描头上。
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
  // mode 是 §7.2 的对照维度：kScan 不刷新新近性、新装入的页优先淘汰；kNormal
  // 与普通访问无异。undo 链的读始终按 kNormal——undo 池没有"普通访问"这回事，
  // 链遍历就是它的正常访问，整片扫成扫描模式会让它的 LRU 失去意义。
  TableScanner(BasePool& base_pool, UndoPool& undo_pool, const TableLayout& layout,
               AccessMode mode = AccessMode::kScan)
      : base_pool_(&base_pool), undo_pool_(&undo_pool), layout_(&layout), mode_(mode) {}

  TableScanner(const TableScanner&) = delete;
  TableScanner& operator=(const TableScanner&) = delete;

  ScanResult ScanBaseline(Timestamp read_ts) {
    ScanResult result;
    const size_t page_count = layout_->page_count();
    const size_t capacity = base_pool_->prefetch_capacity();
    const size_t batch = capacity == 0 ? 1 : capacity;

    for (size_t first = 0; first < page_count; first += batch) {
      const size_t count = std::min(batch, page_count - first);
      if (capacity != 0) {
        base_pool_->Prefetch(static_cast<PageId>(first), count, mode_, &result.base_stats);
      }

      for (size_t index = 0; index < count; ++index) {
        Frame* frame =
            base_pool_->Fix(static_cast<PageId>(first + index), mode_, &result.base_stats);
        const PinGuard<BasePool> pin(*base_pool_, frame);
        const Page page(frame->data, *layout_);
        for (size_t slot = 0; slot < page.row_count(); ++slot) {
          ScanRow(page.RowAt(slot), read_ts, result);
        }
      }
    }

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
      Frame* frame = undo_pool_->Fix(cursor.page(), AccessMode::kNormal, &result.undo_stats);
      const PinGuard<UndoPool> pin(*undo_pool_, frame);
      const Row record = Page(frame->data, *layout_).RowAt(cursor.slot());
      ++result.chain_hops;

      if (record.version().timestamp() < read_ts) {
        result.checksum ^= ChecksumBytes(record.data());
        return;
      }

      cursor = record.undo_row_id();
    }
  }

  BasePool* base_pool_;
  UndoPool* undo_pool_;
  const TableLayout* layout_;
  AccessMode mode_;
};

}  // namespace mvcc
