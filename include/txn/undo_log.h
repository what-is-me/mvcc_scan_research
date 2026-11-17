#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <span>

#include "buffer/frame.h"
#include "common/basic_types.h"
#include "storage/page.h"
#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {

inline constexpr PageId kNoUndoPage = ~PageId{0};

// undo 页号全局取号：单调递增、不回收，因此 undo_row_id 全局唯一。
class UndoPageAllocator {
 public:
  PageId Allocate() { return next_.fetch_add(1, std::memory_order_relaxed); }
  PageId allocated() const { return next_.load(std::memory_order_relaxed); }

 private:
  std::atomic<PageId> next_{0};
};

// 预留好的 undo 槽位，持有该 undo 页的 pin 直到析构。写入时机由调用方决定
// ——必须在行 mutex 内、并且在本对象析构之前，所以这里不替调用方写。
template <typename Pool>
class UndoReservation {
 public:
  UndoReservation(Pool& pool, Frame* frame, Row row, UndoRowId id)
      : pool_(&pool), frame_(frame), row_(row), id_(id) {}

  ~UndoReservation() { pool_->Unpin(frame_); }

  UndoReservation(const UndoReservation&) = delete;
  UndoReservation& operator=(const UndoReservation&) = delete;
  UndoReservation(UndoReservation&&) = delete;
  UndoReservation& operator=(UndoReservation&&) = delete;

  UndoRowId id() const { return id_; }

  // 记录 = {旧版本戳, prev, 旧数据}，与基表行同布局。写入即标脏：这一页在
  // 淘汰时必须落盘，否则记录随帧一起被丢弃。
  void Write(VersionStamp old_version, UndoRowId prev, std::span<const std::byte> old_data) {
    assert(old_data.size() <= row_.data().size());
    row_.set_version(old_version);
    row_.set_undo_row_id(prev);
    std::copy(old_data.begin(), old_data.end(), row_.data().begin());
    pool_->MarkDirty(frame_);
  }

 private:
  Pool* pool_;
  Frame* frame_;
  Row row_;
  UndoRowId id_;
};

// 每个 TP 线程一个：在当前的 undo 页里按槽位顺序追加，页满时向全局分配器取
// 新页号。页内推进是本地的，不需要任何原子操作。
class UndoAppender {
 public:
  UndoAppender(UndoPageAllocator& allocator, const TableLayout& layout)
      : allocator_(&allocator), layout_(&layout) {}

  UndoAppender(const UndoAppender&) = delete;
  UndoAppender& operator=(const UndoAppender&) = delete;

  // 取下一条记录槽位并 pin 住所在页。新分配的页还没落过盘，走 FixEmpty 零填充
  // 而不是读文件；两种情况都可能发生 I/O 或等待，因此必须在行 mutex 之外调用。
  template <typename Pool>
  UndoReservation<Pool> Reserve(Pool& pool) {
    bool fresh = false;
    if (page_ == kNoUndoPage || next_slot_ == layout_->rows_per_page()) {
      page_ = allocator_->Allocate();
      next_slot_ = 0;
      fresh = true;
    }

    Frame* frame = fresh ? pool.FixEmpty(page_, AccessMode::kNormal)
                         : pool.Fix(page_, AccessMode::kNormal);

    const size_t slot = next_slot_++;
    assert(slot <= static_cast<size_t>(SlotId{-1}));
    const UndoRowId id = UndoRowId::Make(page_, static_cast<SlotId>(slot));
    return UndoReservation<Pool>(pool, frame, Page(frame->data, *layout_).RowAt(slot), id);
  }

  PageId current_page() const { return page_; }
  size_t records_on_current_page() const { return next_slot_; }

 private:
  UndoPageAllocator* allocator_;
  const TableLayout* layout_;
  PageId page_ = kNoUndoPage;
  size_t next_slot_ = 0;
};

}  // namespace mvcc
