#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <exception>
#include <ostream>
#include <span>

#include "common/basic_types.h"
#include "common/fixed_array.h"
#include "common/scope_exit.h"
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
        context_(&context),
        pinned_(context.capacity()) {}

  ~Transaction() {
    if (!active_) return;
    // 取号之后中断会让读者看到半个写集，而本系统没有 WAL、也没有恢复路径
    // （§2 非目标）。此时唯一安全的处置是不再回滚并终止，而不是静默产出
    // 错误结果。Commit 已把设备读挡在取号之前，正常路径不会走到这里。
    if (committed_) std::terminate();
    Abort();
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(Transaction&&) = delete;

  void Begin() {
    context_->Reset();
    context_->set_read_ts(timestamps_->ReadTimestamp());
    commit_ts_ = 0;
    committed_ = false;
    active_ = true;
  }

  Timestamp read_ts() const { return context_->read_ts(); }
  Timestamp commit_ts() const { return commit_ts_; }
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
  //
  // 取号之后就不能再失败。此时一部分行已带 commit_ts，一旦中途抛出，槽位随
  // 栈展开释放，读者就能拿到大于该 commit_ts 的 read_ts 而看到半个写集——
  // 正是 §8 的 min 机制要防的撕裂。所以先把写集涉及的页全部装入并 pin 住：
  // 唯一的真实失败源是缺页时的设备读，把它挡在取号之前，失败时可以安全回滚。
  void Commit() {
    assert(active_);
    PinWriteSetPages();
    // 写集页必须无条件归还：CommitScope 构造抛出时 committed_ 还是 false，
    // 析构会走 Abort，而 Abort 不碰 pinned_。
    auto unpin = MakeScopeExit([this] { UnpinWriteSetPages(); });

    const CommitScope scope(*timestamps_, slot_);
    committed_ = true;
    commit_ts_ = scope.commit_ts();
    const VersionStamp stamp = VersionStamp::Committed(commit_ts_);

    // 页面已驻留且被 pin，下面的 Fix 只是命中，不会再读盘，也就不会抛出。
    for (size_t index = 0; index < context_->size(); ++index) {
      RowGuard<BasePool> guard(*base_pool_, *layout_, context_->row_at(index));
      // 写集里的每一行从更新那一刻起就由本事务置锁，直到这里写回 commit_ts。
      // 锁位若已不在，说明有人越过了冲突检查改过这一行，此时覆盖会永久抹掉
      // 对方已提交的版本（读者回溯时直接跳过它）。
      assert(guard.row().version().locked());
      const std::span<const std::byte> fresh = context_->data_at(index);
      std::copy(fresh.begin(), fresh.end(), guard.row().data().begin());
      guard.row().set_version(stamp);
      guard.MarkDirty();
    }

    active_ = false;
    committed_ = false;
  }

  // §9.5：只清锁位，不恢复 undo_row_id——恢复链头会让读者回溯到过老的版本。
  void Abort() {
    assert(active_);
    assert(!committed_ && "取号之后回滚只会留下半个写集");
    for (size_t index = 0; index < context_->size(); ++index) {
      RowGuard<BasePool> guard(*base_pool_, *layout_, context_->row_at(index));
      const VersionStamp current = guard.row().version();
      assert(current.locked());
      if (current.locked()) guard.row().set_version(current.WithoutLock());
      guard.MarkDirty();
    }
    active_ = false;
  }

 private:
  // 把写集涉及的页全部装入并 pin 住。可能触发缺页读，也可能等待淘汰；这一步
  // 失败时什么都没发布，直接抛出即可，析构会照常回滚。
  void PinWriteSetPages() {
    size_t pinned = 0;
    auto rollback = MakeScopeExit([&] {
      while (pinned > 0) base_pool_->Unpin(pinned_[--pinned]);
    });
    for (; pinned < context_->size(); ++pinned) {
      pinned_[pinned] = base_pool_->Fix(context_->row_at(pinned).page(), AccessMode::kNormal);
    }
    rollback.Dismiss();
  }

  void UnpinWriteSetPages() {
    for (size_t index = 0; index < context_->size(); ++index) base_pool_->Unpin(pinned_[index]);
  }

  BasePool* base_pool_;
  UndoPool* undo_pool_;
  const TableLayout* layout_;
  UndoAppender* appender_;
  TimestampManager* timestamps_;
  size_t slot_;
  TxnContext* context_;
  FixedArray<Frame*> pinned_;
  Timestamp commit_ts_ = 0;
  bool active_ = false;
  bool committed_ = false;
};

}  // namespace mvcc
