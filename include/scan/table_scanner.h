#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "buffer/buffer_pool.h"
#include "buffer/pin_guard.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {

// 扫描策略。baseline 与 hotfirst 在主扫描中就地解析版本链，差的只是页的访问
// 次序；两种 defer 把走链整体推迟到第二阶段，主扫描只记录链头。
enum class ScanPolicy : uint8_t { kBaseline, kHotFirst, kDeferChain, kDeferPaged };

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

// defer 的两阶段形态。分开暴露是为了能分别计时：把 undo 访问推迟并批量化
// 的收益，要看主扫描省下多少、解析又补回多少，只给总时长说明不了问题。
struct DeferredScan {
  ScanResult result;
  std::vector<UndoRowId> pending;
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

namespace scan_detail {

// 数据先读，读完放 acquire 栅栏再让调用方读版本戳——这道栅栏挡住"数据读被
// 重排到版本戳之后"，否则可能拿旧凭证配新数据。拆成独立函数是为了让
// "读数据"与"判定 + 取链头"之间可以插入一个写者，把 I2 的窗口测出来。
inline uint64_t FoldBaseData(Row row) {
  const uint64_t fold = ChecksumBytes(row.data());
  std::atomic_thread_fence(std::memory_order_acquire);
  return fold;
}

// 版本戳判定。失败时**此刻**再读一次链头：首次读到的可能还是写者换链头之前
// 的旧值，从它回溯会跳过写者刚追加的那条记录，而那条记录恰好就是快照要的
// 版本。返回 true 表示基表版本可见，fold 有效；否则 cursor 是重读后的链头。
inline bool CheckBaseVersion(Row row, Timestamp read_ts, UndoRowId& cursor) {
  const VersionStamp stamp = row.version();
  if (!stamp.locked() && stamp.timestamp() < read_ts) return true;
  cursor = row.undo_row_id();
  return false;
}

}  // namespace scan_detail

// 全表扫描。三种策略共用页内逐行的可见性判定，差异只在页面访问组织与版本
// 解析时机（§10）：
//   baseline  按页号升序扫，需要旧版本就立即沿 undo 链回溯；
//   hotfirst  先扫扫描开始瞬间驻留的页，再扫其余页，两段都按页号升序；
//   defer     主扫描只记录链头，解析阶段再访问 undo 页（链式或按页分桶）。
// 页按批预取，一次 Fix 覆盖整页的行——逐行 Fix 会给每行再加一次池锁往返。
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

  ScanResult Scan(Timestamp read_ts, ScanPolicy policy) {
    switch (policy) {
      case ScanPolicy::kBaseline:
        return ScanBaseline(read_ts);
      case ScanPolicy::kHotFirst:
        return ScanHotFirst(read_ts);
      case ScanPolicy::kDeferChain: {
        DeferredScan scan = ScanDeferMain(read_ts);
        ResolveDeferredChain(scan, read_ts);
        return scan.result;
      }
      case ScanPolicy::kDeferPaged: {
        DeferredScan scan = ScanDeferMain(read_ts);
        ResolveDeferredPaged(scan, read_ts);
        return scan.result;
      }
    }
    return ScanResult{};
  }

  ScanResult ScanBaseline(Timestamp read_ts) {
    ScanResult result;
    ScanTable(result, [this, read_ts, &result](Row row) { ScanRow(row, read_ts, result); });
    return result;
  }

  // 热点集合在扫描开始这一刻固化：对基表页表做一次快照，热点段 = 快照集合，
  // 冷段 = 全部页号 − 快照集合。不固化的话，扫描自己与 TP 都在持续改变驻留
  // 状态，冷热划分就退化成随机。快照允许近似——并发淘汰只让某一页从热段掉到
  // 冷段，扫到时重新装入，正确性不受影响。
  ScanResult ScanHotFirst(Timestamp read_ts) {
    std::vector<PageId> hot;
    hot.reserve(base_pool_->frame_count());
    base_pool_->CollectResidentPages(hot);
    std::sort(hot.begin(), hot.end());
    // 页表里理论上不会有越界页；真出现了就丢掉，否则冷段归并会卡住，
    // 而 Fix 一个越界页会直接读到文件尾抛异常。
    std::erase_if(hot, [this](PageId page) { return page >= layout_->page_count(); });

    ScanResult result;
    ScanPages(hot, read_ts, result);

    std::vector<PageId> cold;
    // 快照可能含重复页（页表本不该有，但近似快照不依赖这个前提），
    // 按页数上界预留，避免 hot.size() 异常时下溢。
    cold.reserve(layout_->page_count());
    size_t next_hot = 0;
    for (size_t page = 0; page < layout_->page_count(); ++page) {
      if (next_hot < hot.size() && hot[next_hot] == page) {
        ++next_hot;
        continue;
      }
      cold.push_back(static_cast<PageId>(page));
    }
    ScanPages(cold, read_ts, result);
    return result;
  }

  // defer 第一阶段：与 baseline 完全相同的页访问组织与可见性判定，只把"走链"
  // 换成"记录重读后的链头"。全程不触碰 undo 池。
  DeferredScan ScanDeferMain(Timestamp read_ts) {
    DeferredScan scan;
    ScanTable(scan.result, [this, read_ts, &scan](Row row) {
      ScanRowDefer(row, read_ts, scan.result, scan.pending);
    });
    return scan;
  }

  // defer 第二阶段：每条链独立回溯。页访问次序由链的自然顺序决定，同一页会被
  // 不同链反复装入——这是"链式"作为对照组的代价。
  void ResolveDeferredChain(DeferredScan& scan, Timestamp read_ts) {
    for (const UndoRowId cursor : scan.pending) {
      ResolveChain(cursor, read_ts, scan.result);
    }
    scan.pending.clear();
  }

  // defer 第二阶段：把待解析的链头按 undo 页号分桶，每桶只装入一次，页内批量
  // 解析；解析中新产生的 prev 若在别的页，收集进下一轮，迭代至无待解析项。
  // 同一页上的续跳就地走完，不再攒到下一轮——同一行被同一线程连续更新时，
  // 整条链常常落在同一页，这是页式相对链式的主要收益。
  //
  // 安全性来自 undo 记录不可变且只追加：主扫描记录链头之后链可能继续增长，
  // 但已存在的节点不变，回溯的终止条件也不受影响。
  void ResolveDeferredPaged(DeferredScan& scan, Timestamp read_ts) {
    std::vector<UndoRowId> pending = std::move(scan.pending);
    std::vector<UndoRowId> next;

    while (!pending.empty()) {
      std::sort(pending.begin(), pending.end(), [](UndoRowId left, UndoRowId right) {
        if (left.page() != right.page()) return left.page() < right.page();
        return left.slot() < right.slot();
      });

      next.clear();
      for (size_t index = 0; index < pending.size();) {
        const PageId id = pending[index].page();
        Frame* frame = undo_pool_->Fix(id, AccessMode::kNormal, &scan.result.undo_stats);
        const PinGuard<UndoPool> pin(*undo_pool_, frame);
        const Page undo_page(frame->data, *layout_);

        size_t end = index;
        while (end < pending.size() && pending[end].page() == id) {
          UndoRowId cursor = pending[end++];
          for (;;) {
            assert(!cursor.is_null());
            const Row record = undo_page.RowAt(cursor.slot());
            ++scan.result.chain_hops;
            if (record.version().timestamp() < read_ts) {
              scan.result.checksum ^= ChecksumBytes(record.data());
              break;
            }
            const UndoRowId previous = record.undo_row_id();
            if (previous.page() != id) {
              next.push_back(previous);
              break;
            }
            cursor = previous;
          }
        }
        index = end;
      }
      pending.swap(next);
    }
    scan.pending.clear();
  }

 private:
  // 按页号升序扫全表，相邻页成批预取。
  template <typename OnRow>
  void ScanTable(ScanResult& result, OnRow on_row) {
    const size_t page_count = layout_->page_count();
    const size_t capacity = base_pool_->prefetch_capacity();
    const size_t batch = capacity == 0 ? 1 : capacity;

    for (size_t first = 0; first < page_count; first += batch) {
      const size_t count = std::min(batch, page_count - first);
      if (capacity != 0) {
        base_pool_->Prefetch(static_cast<PageId>(first), count, mode_, &result.base_stats);
      }
      for (size_t index = 0; index < count; ++index) {
        ScanPage(static_cast<PageId>(first + index), result, on_row);
      }
    }
  }

  // 按给定页号列表扫描，列表须升序。段内连续处成批预取，有洞就断开——
  // hotfirst 的热点段通常稀疏，冷段则是一整段连续页号。
  void ScanPages(std::span<const PageId> pages, Timestamp read_ts, ScanResult& result) {
    const auto scan_row = [this, read_ts, &result](Row row) { ScanRow(row, read_ts, result); };
    const size_t capacity = base_pool_->prefetch_capacity();
    size_t index = 0;
    while (index < pages.size()) {
      size_t run = 1;
      if (capacity != 0) {
        while (run < capacity && index + run < pages.size() &&
               pages[index + run] == pages[index + run - 1] + 1) {
          ++run;
        }
        base_pool_->Prefetch(pages[index], run, mode_, &result.base_stats);
      }
      for (size_t offset = 0; offset < run; ++offset) {
        ScanPage(pages[index + offset], result, scan_row);
      }
      index += run;
    }
  }

  template <typename OnRow>
  void ScanPage(PageId id, ScanResult& result, OnRow& on_row) {
    Frame* frame = base_pool_->Fix(id, mode_, &result.base_stats);
    const PinGuard<BasePool> pin(*base_pool_, frame);
    const Page page(frame->data, *layout_);
    for (size_t slot = 0; slot < page.row_count(); ++slot) {
      on_row(page.RowAt(slot));
    }
  }

  void ScanRow(Row row, Timestamp read_ts, ScanResult& result) {
    const uint64_t fold = scan_detail::FoldBaseData(row);
    UndoRowId cursor = RowId::Null();
    const bool visible = scan_detail::CheckBaseVersion(row, read_ts, cursor);
    ++result.rows;

    if (visible) {
      result.checksum ^= fold;
      return;
    }
    ++result.chain_rows;
    ResolveChain(cursor, read_ts, result);
  }

  void ScanRowDefer(Row row, Timestamp read_ts, ScanResult& result,
                    std::vector<UndoRowId>& pending) {
    const uint64_t fold = scan_detail::FoldBaseData(row);
    UndoRowId cursor = RowId::Null();
    const bool visible = scan_detail::CheckBaseVersion(row, read_ts, cursor);
    ++result.rows;

    if (visible) {
      result.checksum ^= fold;
      return;
    }
    ++result.chain_rows;
    pending.push_back(cursor);
  }

  // 从链头回溯到第一条 ts < read_ts 的记录。链尾是版本戳 0，而 read_ts >= 1，
  // 所以一定能停下来；写者只改链头、不动旧节点，沿途不会读到撕裂的记录。
  void ResolveChain(UndoRowId cursor, Timestamp read_ts, ScanResult& result) {
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
