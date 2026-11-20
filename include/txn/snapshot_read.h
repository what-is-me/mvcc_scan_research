#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <span>

#include "buffer/frame.h"
#include "buffer/pin_guard.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/table_layout.h"

namespace mvcc {

// 快照读（§9.2）：把某行对 read_ts 可见的版本数据拷进 out。
//
// 顺序是协议的一部分：数据 -> undo_row_id -> 版本戳。版本戳是"读到的内容是否
// 有效"的凭证，必须最后读；判定成立后还要重读 undo_row_id，因为首次读到的
// 可能还是写者更新链头之前的旧值（I2）。
//
// 这次拷贝与提交并发时可能是写者改到一半的中间态——它是有意的乐观读，判定
// 失败即整份丢弃、改走版本链，不会返回给调用方（I7）。
template <typename BasePool, typename UndoPool>
void ReadVisible(BasePool& base_pool, UndoPool& undo_pool, const TableLayout& layout, RowId id,
                 Timestamp read_ts, AccessMode mode, std::span<std::byte> out) {
  assert(out.size() >= layout.data_size());

  UndoRowId cursor = RowId::Null();
  {
    Frame* frame = base_pool.Fix(id.page(), mode);
    const PinGuard<BasePool> pin(base_pool, frame);
    const Row row = Page(frame->data, layout).RowAt(id.slot());

    std::copy(row.data().begin(), row.data().end(), out.begin());
    cursor = row.undo_row_id();
    // 与扫描器相同：数据读完之后放 acquire 栅栏，再读版本戳。
    std::atomic_thread_fence(std::memory_order_acquire);
    const VersionStamp stamp = row.version();

    if (!stamp.locked() && stamp.timestamp() < read_ts) return;

    cursor = row.undo_row_id();
  }

  // 版本链必然在第一条 ts < read_ts 的记录处终止：链尾是版本戳 0，而 read_ts >= 1。
  for (;;) {
    Frame* frame = undo_pool.Fix(cursor.page(), AccessMode::kNormal);
    const PinGuard<UndoPool> pin(undo_pool, frame);
    const Row record = Page(frame->data, layout).RowAt(cursor.slot());
    if (record.version().timestamp() < read_ts) {
      std::copy(record.data().begin(), record.data().end(), out.begin());
      return;
    }
    cursor = record.undo_row_id();
  }
}

}  // namespace mvcc
