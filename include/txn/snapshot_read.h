#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>

#include "buffer/frame.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/table_layout.h"

namespace mvcc {

// 快照读（§9.2）：把某行对 read_ts 可见的版本数据拷进 out。
//
// 顺序是协议的一部分：数据 -> undo_row_id -> 版本戳。版本戳是"读到的内容是否
// 有效"的凭证，必须最后读；判定成立后还要重读 undo_row_id，因为首次读到的
// 可能还是写者更新链头之前的旧值（I2）。
template <typename BasePool, typename UndoPool>
void ReadVisible(BasePool& base_pool, UndoPool& undo_pool, const TableLayout& layout, RowId id,
                 Timestamp read_ts, AccessMode mode, std::span<std::byte> out) {
  assert(out.size() >= layout.data_size());

  UndoRowId cursor = RowId::Null();
  {
    Frame* frame = base_pool.Fix(id.page(), mode);
    const Row row = Page(frame->data, layout).RowAt(id.slot());

    std::copy(row.data().begin(), row.data().end(), out.begin());
    cursor = row.undo_row_id();
    const VersionStamp stamp = row.version();

    if (!stamp.locked() && stamp.timestamp() < read_ts) {
      base_pool.Unpin(frame);
      return;
    }

    cursor = row.undo_row_id();
    base_pool.Unpin(frame);
  }

  // 版本链必然在第一条 ts < read_ts 的记录处终止：链尾是版本戳 0，而 read_ts >= 1。
  for (;;) {
    Frame* frame = undo_pool.Fix(cursor.page(), AccessMode::kNormal);
    const Row record = Page(frame->data, layout).RowAt(cursor.slot());
    if (record.version().timestamp() < read_ts) {
      std::copy(record.data().begin(), record.data().end(), out.begin());
      undo_pool.Unpin(frame);
      return;
    }
    cursor = record.undo_row_id();
    undo_pool.Unpin(frame);
  }
}

}  // namespace mvcc
