#pragma once

#include <cassert>
#include <mutex>

#include "buffer/frame.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {

// 行守卫：pin 基表页 -> 取该槽位的行 mutex -> 上锁。
//
// 析构顺序反过来，先放锁再 unpin。反过来的话，帧可能在两步之间被淘汰并在别处
// 重装，锁就落在了一个已经换了内容的位置上。持锁期间 pin 恒 >= 1，因此帧不会
// 被淘汰，帧内的行 mutex 数组也不会失效。
template <typename Pool>
class RowGuard {
 public:
  RowGuard(Pool& pool, const TableLayout& layout, RowId id,
           AccessMode mode = AccessMode::kNormal)
      : pool_(&pool),
        id_(id),
        frame_(pool.Fix(id.page(), mode)),
        lock_(&LockOf(frame_, id.slot())),
        row_(Page(frame_->data, layout).RowAt(id.slot())) {
    try {
      lock_->lock();
    } catch (...) {
      // 构造没有完成就不会走析构，已拿到的 pin 必须在这里还回去，否则这一帧
      // 永远 pin > 0，再也选不成牺牲帧。
      pool_->Unpin(frame_);
      throw;
    }
  }

  ~RowGuard() {
    lock_->unlock();
    pool_->Unpin(frame_);
  }

  RowGuard(const RowGuard&) = delete;
  RowGuard& operator=(const RowGuard&) = delete;
  RowGuard(RowGuard&&) = delete;
  RowGuard& operator=(RowGuard&&) = delete;

  Row row() const { return row_; }
  RowId id() const { return id_; }
  Frame* frame() const { return frame_; }

  // 改过行内容后必须调用，否则该页可能被当作干净页淘汰而丢掉修改。守卫本身
  // 不自动标脏：同一把守卫也用于只读的冲突检查与快照读。
  void MarkDirty() { pool_->MarkDirty(frame_); }

 private:
  static std::mutex& LockOf(Frame* frame, SlotId slot) {
    assert(slot < frame->row_locks.size());
    return frame->row_locks[slot];
  }

  Pool* pool_;
  RowId id_;
  Frame* frame_;
  std::mutex* lock_;
  Row row_;
};

}  // namespace mvcc
