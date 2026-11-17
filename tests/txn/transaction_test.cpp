#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "storage/page.h"
#include "storage/table_layout.h"
#include "txn/row_guard.h"
#include "txn/snapshot_read.h"
#include "txn/transaction.h"
#include "txn/txn_context.h"

#include "../buffer/memory_page_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kTableRows = 1024;
constexpr size_t kUndoPages = 256;
constexpr size_t kBaseFrames = 64;
constexpr size_t kUndoFrames = 16;
constexpr size_t kRowsPerTxn = 4;

using BasePool = BufferPool<DirectPageTable, LruReplacer>;
using UndoPool = BufferPool<FrameHashTable, LruReplacer>;
using Txn = Transaction<BasePool, UndoPool>;

struct Env {
  TableLayout layout{kPageSize, kRowSize, kTableRows};
  test::MemoryPageFile base_file{layout.page_count(), layout.page_size(), test::PageFill::kZero};
  test::MemoryPageFile undo_file{kUndoPages, layout.page_size(), test::PageFill::kZero};
  BasePool base{layout, kBaseFrames, base_file, RowLockMode::kAllocate,
                DirectPageTable(layout.page_count()), LruReplacer(kBaseFrames)};
  UndoPool undo{layout, kUndoFrames, undo_file, RowLockMode::kNone, FrameHashTable(kUndoFrames),
                LruReplacer(kUndoFrames)};
  UndoPageAllocator allocator;
  UndoAppender appender{allocator, layout};
  TimestampManager timestamps{kRowsPerTxn};
  TxnContext context_a{kRowsPerTxn, layout.data_size()};
  TxnContext context_b{kRowsPerTxn, layout.data_size()};
};

std::vector<std::byte> DataWith(uint8_t seed, size_t size) {
  return std::vector<std::byte>(size, static_cast<std::byte>(seed));
}

std::vector<std::byte> ReadRow(Env& env, RowId id, Timestamp read_ts) {
  std::vector<std::byte> out(env.layout.data_size());
  ReadVisible(env.base, env.undo, env.layout, id, read_ts, AccessMode::kNormal, out);
  return out;
}

TEST(Transaction, CommitMakesUpdateVisible) {
  Env env;
  const RowId row = RowId::Make(0, 3);
  const std::vector<std::byte> zeros(env.layout.data_size(), std::byte{0});
  const std::vector<std::byte> updated = DataWith(0xAA, env.layout.data_size());

  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), zeros);

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  txn.Begin();
  EXPECT_EQ(txn.Update(row, updated), UpdateOutcome::kApplied);
  txn.Commit();
  EXPECT_FALSE(txn.active());

  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), updated);
}

TEST(Transaction, AbortRestoresValueWithoutRestoringChainHead) {
  Env env;
  const RowId row = RowId::Make(1, 2);
  const std::vector<std::byte> zeros(env.layout.data_size(), std::byte{0});

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  txn.Begin();
  EXPECT_EQ(txn.Update(row, DataWith(0x55, env.layout.data_size())), UpdateOutcome::kApplied);
  txn.Abort();
  EXPECT_FALSE(txn.active());

  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), zeros);

  const RowGuard guard(env.base, env.layout, row);
  EXPECT_FALSE(guard.row().version().locked());
  EXPECT_EQ(guard.row().version().timestamp(), Timestamp{0});
  EXPECT_NE(guard.row().undo_row_id(), RowId::Null()) << "回滚不恢复链头";
}

TEST(Transaction, RepeatedUpdateCollapsesIntoOneWriteSetEntry) {
  Env env;
  const RowId row = RowId::Make(2, 1);
  const std::vector<std::byte> last = DataWith(0xBB, env.layout.data_size());

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  txn.Begin();
  EXPECT_EQ(txn.Update(row, DataWith(0xAA, env.layout.data_size())), UpdateOutcome::kApplied);
  EXPECT_EQ(txn.Update(row, last), UpdateOutcome::kApplied);
  EXPECT_EQ(txn.context().size(), 1u);
  txn.Commit();

  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), last);
}

TEST(Transaction, ConflictsWithRowLockedByAnotherTransaction) {
  Env env;
  const RowId row = RowId::Make(3, 0);
  const std::vector<std::byte> data = DataWith(0x11, env.layout.data_size());

  Txn first(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  Txn second(env.base, env.undo, env.layout, env.appender, env.timestamps, 1, env.context_b);

  first.Begin();
  EXPECT_EQ(first.Update(row, data), UpdateOutcome::kApplied);

  second.Begin();
  EXPECT_EQ(second.Update(row, data), UpdateOutcome::kConflict);

  first.Abort();
  second.Abort();
}

TEST(Transaction, ConflictsWithCommitThatHappenedAfterReadTs) {
  Env env;
  const RowId row = RowId::Make(4, 4);
  const std::vector<std::byte> data = DataWith(0x22, env.layout.data_size());

  Txn late(env.base, env.undo, env.layout, env.appender, env.timestamps, 1, env.context_b);
  Txn early(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);

  late.Begin();
  const Timestamp read_ts = late.read_ts();

  early.Begin();
  ASSERT_EQ(early.Update(row, data), UpdateOutcome::kApplied);
  early.Commit();

  EXPECT_GE(env.timestamps.counter() - 1, read_ts);
  EXPECT_EQ(late.Update(row, data), UpdateOutcome::kConflict);
  late.Abort();
}

TEST(Transaction, SnapshotReadHidesCommitAfterReadTimestamp) {
  Env env;
  const RowId row = RowId::Make(5, 7);
  const std::vector<std::byte> zeros(env.layout.data_size(), std::byte{0});
  const std::vector<std::byte> updated = DataWith(0x33, env.layout.data_size());

  const Timestamp before = env.timestamps.ReadTimestamp();

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  txn.Begin();
  ASSERT_EQ(txn.Update(row, updated), UpdateOutcome::kApplied);
  txn.Commit();

  EXPECT_EQ(ReadRow(env, row, before), zeros);
  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), updated);
}

TEST(Transaction, VersionChainServesIntermediateReadTimestamp) {
  Env env;
  const RowId row = RowId::Make(6, 0);
  const std::vector<std::byte> first_data = DataWith(0x44, env.layout.data_size());
  const std::vector<std::byte> second_data = DataWith(0x66, env.layout.data_size());

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  txn.Begin();
  ASSERT_EQ(txn.Update(row, first_data), UpdateOutcome::kApplied);
  txn.Commit();

  const Timestamp middle = env.timestamps.ReadTimestamp();

  txn.Begin();
  ASSERT_EQ(txn.Update(row, second_data), UpdateOutcome::kApplied);
  txn.Commit();

  EXPECT_EQ(ReadRow(env, row, middle), first_data);
  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), second_data);
}

TEST(Transaction, DestructorRollsBackActiveTransaction) {
  Env env;
  const RowId row = RowId::Make(7, 1);
  const std::vector<std::byte> zeros(env.layout.data_size(), std::byte{0});

  {
    Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
    txn.Begin();
    ASSERT_EQ(txn.Update(row, DataWith(0x77, env.layout.data_size())), UpdateOutcome::kApplied);
  }

  EXPECT_EQ(ReadRow(env, row, env.timestamps.ReadTimestamp()), zeros);
  const RowGuard guard(env.base, env.layout, row);
  EXPECT_FALSE(guard.row().version().locked());
}

TEST(Transaction, ReportsWriteSetFull) {
  Env env;
  const std::vector<std::byte> data = DataWith(0x88, env.layout.data_size());

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context_a);
  txn.Begin();
  for (size_t index = 0; index < kRowsPerTxn; ++index) {
    EXPECT_EQ(txn.Update(RowId::Make(8, static_cast<SlotId>(index)), data),
              UpdateOutcome::kApplied);
  }
  EXPECT_EQ(txn.Update(RowId::Make(8, static_cast<SlotId>(kRowsPerTxn)), data),
            UpdateOutcome::kWriteSetFull);
  txn.Abort();
}

// 每个线程只碰自己那一组行，所以不会冲突；这样跑到的是真正并发的更新、
// 提交与 undo 页换入换出，而不是冲突退避。
TEST(Transaction, ConcurrentDisjointUpdatesAllLand) {
  constexpr size_t kThreads = 4;
  constexpr size_t kIterations = 50;

  Env env;
  TimestampManager timestamps{kThreads};
  std::vector<TxnContext> contexts;
  contexts.reserve(kThreads);
  for (size_t index = 0; index < kThreads; ++index) {
    contexts.emplace_back(kRowsPerTxn, env.layout.data_size());
  }

  auto row_of = [&env](size_t thread, size_t iteration) {
    const size_t linear = thread * kIterations + iteration;
    return RowId::Make(linear / env.layout.rows_per_page(),
                       static_cast<SlotId>(linear % env.layout.rows_per_page()));
  };
  auto data_of = [&env](size_t thread, size_t iteration) {
    return DataWith(static_cast<uint8_t>(thread * kIterations + iteration + 1),
                    env.layout.data_size());
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      UndoAppender appender(env.allocator, env.layout);
      Txn txn(env.base, env.undo, env.layout, appender, timestamps, thread, contexts[thread]);
      for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        txn.Begin();
        EXPECT_EQ(txn.Update(row_of(thread, iteration), data_of(thread, iteration)),
                  UpdateOutcome::kApplied);
        txn.Commit();
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  const Timestamp now = timestamps.ReadTimestamp();
  for (size_t thread = 0; thread < kThreads; ++thread) {
    for (size_t iteration = 0; iteration < kIterations; ++iteration) {
      EXPECT_EQ(ReadRow(env, row_of(thread, iteration), now), data_of(thread, iteration))
          << "thread " << thread << " iteration " << iteration;
    }
  }
}

}  // namespace
}  // namespace mvcc
