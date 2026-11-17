#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <ostream>
#include <span>

#include "common/basic_types.h"
#include "storage/table_layout.h"
#include "txn/row_guard.h"
#include "txn/timestamp_manager.h"
#include "txn/txn_context.h"
#include "txn/undo_log.h"

namespace mvcc {

enum class UpdateOutcome { kApplied, kConflict, kWriteSetFull };

inline std::ostream& operator<<(std::ostream& out, UpdateOutcome outcome) {
  switch (outcome) {
    case UpdateOutcome::kApplied:
      return out << "kApplied";
    case UpdateOutcome::kConflict:
      return out << "kConflict";
    case UpdateOutcome::kWriteSetFull:
      return out << "kWriteSetFull";
  }
  return out << "UpdateOutcome{?}";
}

// 一次事务的编排（§9.3–§9.6）。生命周期短；析构时若仍 active 则自动回滚。
// 它持有的都是 Env 组件的引用，不拥有任何东西。
template <typename BasePool, typename UndoPool>
class Transaction {
 public:
  Transaction(BasePool& base_pool, UndoPool& undo_pool, const TableLayout& layout,
              UndoAppender& appender, TimestampManager& timestamps, size_t slot,
              TxnContext& context)
      : base_pool_(&base_pool),
        undo_pool_(&undo_pool),
        layout_(&layout),
        appender_(&appender),
        timestamps_(&timestamps),
        slot_(slot),
        context_(&context) {}

  ~Transaction() {
    if (active_) Abort();
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(Transaction&&) = delete;

  void Begin() {
    context_->Reset();
    context_->set_read_ts(timestamps_->ReadTimestamp());
    active_ = true;
  }

  Timestamp read_ts() const { return context_->read_ts(); }
  bool active() const { return active_; }
  const TxnContext& context() const { return *context_; }

  // 事务内首次触及某行时按 §9.3 的顺序走；已在本事务写集里的行只替换新数据。
  UpdateOutcome Update(RowId id, std::span<const std::byte> new_data) {
    assert(active_);
    assert(new_data.size() == context_->data_size());

    if (const size_t existing = context_->Find(id); existing != TxnContext::kNoEntry) {
      std::copy(new_data.begin(), new_data.end(), context_->mutable_data_at(existing).begin());
      return UpdateOutcome::kApplied;
    }
    if (context_->full()) return UpdateOutcome::kWriteSetFull;

    // 1. 先占 undo 槽位并 pin 住 undo 页：这一步可能读盘或等待，必须在行
    //    mutex 之外完成。
    auto reservation = appender_->Reserve(*undo_pool_);

    {
      // 2. pin 基表页 + 行 mutex。
      RowGuard<BasePool> guard(*base_pool_, *layout_, id);

      // 3. 冲突检查：已有未决写入，或读取后又被别人提交过。
      const VersionStamp current = guard.row().version();
      if (current.locked() || current.timestamp() >= context_->read_ts()) {
        return UpdateOutcome::kConflict;
      }

      // 4-6. 写 undo 记录 -> 更新链头 -> 置锁，顺序不可调换（I5）。
      reservation.Write(current, guard.row().undo_row_id(), guard.row().data());
      guard.row().set_undo_row_id(reservation.id());
      guard.row().set_version(current.WithLock());
      guard.MarkDirty();
    }

    // 7. 守卫已析构（先放锁后 unpin），reservation 在本函数结束时 unpin undo 页。
    // 8. 追加写集。
    const size_t index = context_->Append(id);
    std::copy(new_data.begin(), new_data.end(), context_->mutable_data_at(index).begin());
    return UpdateOutcome::kApplied;
  }

  // §9.4：先取号并发布，再逐行写数据、写版本戳，最后释放槽位。
  void Commit() {
    assert(active_);
    const CommitScope scope(*timestamps_, slot_);
    const VersionStamp committed = VersionStamp::Committed(scope.commit_ts());

    for (size_t index = 0; index < context_->size(); ++index) {
      RowGuard<BasePool> guard(*base_pool_, *layout_, context_->row_at(index));
      const std::span<const std::byte> data = context_->data_at(index);
      std::copy(data.begin(), data.end(), guard.row().data().begin());
      guard.row().set_version(committed);
      guard.MarkDirty();
    }

    active_ = false;
  }

  // §9.5：只清锁位，不恢复 undo_row_id——恢复链头会让读者回溯到过老的版本。
  void Abort() {
    assert(active_);
    for (size_t index = 0; index < context_->size(); ++index) {
      RowGuard<BasePool> guard(*base_pool_, *layout_, context_->row_at(index));
      const VersionStamp current = guard.row().version();
      if (current.locked()) guard.row().set_version(current.WithoutLock());
      guard.MarkDirty();
    }
    active_ = false;
  }

 private:
  BasePool* base_pool_;
  UndoPool* undo_pool_;
  const TableLayout* layout_;
  UndoAppender* appender_;
  TimestampManager* timestamps_;
  size_t slot_;
  TxnContext* context_;
  bool active_ = false;
};

}  // namespace mvcc
