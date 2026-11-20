#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "scan/table_scanner.h"
#include "storage/page.h"
#include "storage/table_builder.h"
#include "storage/table_layout.h"
#include "txn/row_guard.h"
#include "txn/transaction.h"
#include "txn/txn_context.h"

#include "../buffer/memory_page_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kTableRows = 1024;
constexpr size_t kUndoPages = 64;
constexpr size_t kRowsPerTxn = 4;

using BasePool = BufferPool<DirectPageTable, LruReplacer>;
using UndoPool = BufferPool<FrameHashTable, LruReplacer>;
using Txn = Transaction<BasePool, UndoPool>;

struct Env {
  explicit Env(size_t table_rows = kTableRows, size_t base_frames = 8, size_t undo_frames = 8,
               size_t prefetch = 8)
      : layout(kPageSize, kRowSize, table_rows),
        base_file(layout.page_count(), layout.page_size(), test::PageFill::kZero),
        undo_file(kUndoPages, layout.page_size(), test::PageFill::kZero),
        base(layout, base_frames, base_file, RowLockMode::kAllocate,
             DirectPageTable(layout.page_count()), LruReplacer(base_frames), PoolHooks(), prefetch),
        undo(layout, undo_frames, undo_file, RowLockMode::kNone, FrameHashTable(undo_frames),
             LruReplacer(undo_frames)),
        appender(allocator, layout),
        timestamps(kRowsPerTxn),
        context(kRowsPerTxn, layout.data_size()),
        scanner(base, undo, layout) {}

  TableLayout layout;
  test::MemoryPageFile base_file;
  test::MemoryPageFile undo_file;
  BasePool base;
  UndoPool undo;
  UndoPageAllocator allocator;
  UndoAppender appender;
  TimestampManager timestamps;
  TxnContext context;
  TableScanner<BasePool, UndoPool> scanner;
};

std::vector<std::byte> RowData(size_t linear_row, size_t data_size) {
  std::vector<std::byte> data(data_size);
  for (size_t word = 0; word < data_size / sizeof(uint64_t); ++word) {
    const uint64_t value = RowDataWord(linear_row, word);
    std::memcpy(data.data() + word * sizeof(uint64_t), &value, sizeof(value));
  }
  return data;
}

// 按建表规则写满基表文件：每行数据由行号派生，版本戳与链头保持全零。
void FillDerivedRows(Env& env) {
  for (size_t page_index = 0; page_index < env.layout.page_count(); ++page_index) {
    const Page page(env.base_file.Page(page_index), env.layout);
    for (size_t slot = 0; slot < page.row_count(); ++slot) {
      const size_t linear = page_index * env.layout.rows_per_page() + slot;
      const Row row = page.RowAt(slot);
      for (size_t word = 0; word < env.layout.data_size() / sizeof(uint64_t); ++word) {
        const uint64_t value = RowDataWord(linear, word);
        std::memcpy(row.data().data() + word * sizeof(uint64_t), &value, sizeof(value));
      }
    }
  }
}

uint64_t ExpectedChecksum(const TableLayout& layout) {
  uint64_t checksum = 0;
  for (size_t row = 0; row < layout.row_count(); ++row) {
    checksum ^= ChecksumBytes(RowData(row, layout.data_size()));
  }
  return checksum;
}

void UpdateRow(Env& env, RowId id, std::span<const std::byte> data, bool commit) {
  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context);
  txn.Begin();
  ASSERT_EQ(txn.Update(id, data), UpdateOutcome::kApplied);
  if (commit) {
    txn.Commit();
  } else {
    txn.Abort();
  }
}

TEST(TableScanner, BaselineReadsEveryRowOfAFreshTable) {
  Env env;
  FillDerivedRows(env);

  const ScanResult result = env.scanner.ScanBaseline(env.timestamps.ReadTimestamp());

  EXPECT_EQ(result.rows, env.layout.row_count());
  EXPECT_EQ(result.chain_rows, 0u);
  EXPECT_EQ(result.chain_hops, 0u);
  EXPECT_EQ(result.checksum, ExpectedChecksum(env.layout));
}

TEST(TableScanner, CoversPaddingRowsOfAPartiallyFilledLastPage) {
  Env env(1000);
  FillDerivedRows(env);
  ASSERT_EQ(env.layout.page_count(), 32u);
  ASSERT_EQ(env.layout.row_count(), 1024u);

  const ScanResult result = env.scanner.ScanBaseline(env.timestamps.ReadTimestamp());

  EXPECT_EQ(result.rows, 1024u);
  EXPECT_EQ(result.checksum, ExpectedChecksum(env.layout));
}

TEST(TableScanner, UncommittedUpdateDoesNotLeakIntoTheSnapshot) {
  Env env;
  FillDerivedRows(env);
  const uint64_t original = ExpectedChecksum(env.layout);

  const RowId row = RowId::Make(3, 7);
  const std::vector<std::byte> updated(env.layout.data_size(), std::byte{0xAA});

  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context);
  txn.Begin();
  ASSERT_EQ(txn.Update(row, updated), UpdateOutcome::kApplied);

  // 行已置锁、链头已指向新记录，但数据只改到工作区，扫描必须看到旧值。
  const ScanResult result = env.scanner.ScanBaseline(env.timestamps.ReadTimestamp());

  EXPECT_EQ(result.chain_rows, 1u);
  EXPECT_EQ(result.chain_hops, 1u);
  EXPECT_EQ(result.checksum, original);

  txn.Abort();
}

TEST(TableScanner, CommittedUpdateIsVisibleOnlyToLaterSnapshots) {
  Env env;
  FillDerivedRows(env);
  const uint64_t original = ExpectedChecksum(env.layout);

  const RowId row = RowId::Make(5, 11);
  const size_t linear = row.page() * env.layout.rows_per_page() + row.slot();
  const std::vector<std::byte> updated(env.layout.data_size(), std::byte{0x5A});

  const Timestamp before = env.timestamps.ReadTimestamp();
  UpdateRow(env, row, updated, /*commit=*/true);
  const Timestamp after = env.timestamps.ReadTimestamp();

  const ScanResult old_snapshot = env.scanner.ScanBaseline(before);
  EXPECT_EQ(old_snapshot.chain_rows, 1u);
  EXPECT_EQ(old_snapshot.chain_hops, 1u);
  EXPECT_EQ(old_snapshot.checksum, original);

  const ScanResult new_snapshot = env.scanner.ScanBaseline(after);
  EXPECT_EQ(new_snapshot.chain_rows, 0u);
  EXPECT_EQ(new_snapshot.checksum,
            original ^ ChecksumBytes(RowData(linear, env.layout.data_size())) ^
                ChecksumBytes(updated));
}

TEST(TableScanner, WalksEveryUndoRecordBetweenSnapshotAndHead) {
  Env env;
  FillDerivedRows(env);
  const uint64_t original = ExpectedChecksum(env.layout);

  const RowId row = RowId::Make(2, 1);
  const Timestamp before = env.timestamps.ReadTimestamp();
  for (uint8_t seed = 1; seed <= 3; ++seed) {
    const std::vector<std::byte> data(env.layout.data_size(), static_cast<std::byte>(seed));
    UpdateRow(env, row, data, /*commit=*/true);
  }

  const ScanResult result = env.scanner.ScanBaseline(before);

  EXPECT_EQ(result.chain_rows, 1u);
  EXPECT_EQ(result.chain_hops, 3u);
  EXPECT_EQ(result.checksum, original);
}

TEST(TableScanner, StopsAtTheFirstRecordOlderThanTheSnapshot) {
  Env env;
  FillDerivedRows(env);

  const RowId row = RowId::Make(1, 4);
  const size_t linear = row.page() * env.layout.rows_per_page() + row.slot();
  const std::vector<std::byte> first(env.layout.data_size(), std::byte{0x11});
  const std::vector<std::byte> second(env.layout.data_size(), std::byte{0x22});

  UpdateRow(env, row, first, /*commit=*/true);
  const Timestamp middle = env.timestamps.ReadTimestamp();
  UpdateRow(env, row, second, /*commit=*/true);

  const ScanResult result = env.scanner.ScanBaseline(middle);

  EXPECT_EQ(result.chain_rows, 1u);
  EXPECT_EQ(result.chain_hops, 1u);
  EXPECT_EQ(result.checksum, ExpectedChecksum(env.layout) ^
                                 ChecksumBytes(RowData(linear, env.layout.data_size())) ^
                                 ChecksumBytes(first));
}

TEST(TableScanner, PrefetchCutsReadCallsWithoutChangingTheResult) {
  Env plain(kTableRows, 8, 8, /*prefetch=*/0);
  Env batched(kTableRows, 8, 8, /*prefetch=*/8);
  FillDerivedRows(plain);
  FillDerivedRows(batched);

  const ScanResult a = plain.scanner.ScanBaseline(plain.timestamps.ReadTimestamp());
  const ScanResult b = batched.scanner.ScanBaseline(batched.timestamps.ReadTimestamp());

  EXPECT_EQ(a.checksum, b.checksum);
  EXPECT_EQ(a.rows, b.rows);
  EXPECT_EQ(a.base_stats.physical_reads, b.base_stats.physical_reads);
  EXPECT_EQ(a.base_stats.physical_read_ops, plain.layout.page_count());
  EXPECT_LT(b.base_stats.physical_read_ops, a.base_stats.physical_read_ops);
  EXPECT_EQ(b.base_stats.physical_read_ops, plain.layout.page_count() / 8);

  // 池装不下整表时命中率必须体现出来：逐页扫描全miss，成批预取一半命中。
  EXPECT_DOUBLE_EQ(a.base_stats.hit_rate(), 0.0);
  EXPECT_DOUBLE_EQ(b.base_stats.hit_rate(), 0.5);
}

TEST(TableScanner, SurvivesWhenThePoolIsSmallerThanTheTable) {
  Env env(kTableRows, /*base_frames=*/2, /*undo_frames=*/2, /*prefetch=*/4);
  FillDerivedRows(env);

  for (uint8_t seed = 1; seed <= 4; ++seed) {
    const std::vector<std::byte> data(env.layout.data_size(), static_cast<std::byte>(seed));
    UpdateRow(env, RowId::Make(seed, seed), data, /*commit=*/true);
  }

  const ScanResult result = env.scanner.ScanBaseline(env.timestamps.ReadTimestamp());

  EXPECT_EQ(result.rows, env.layout.row_count());
  // 池只有 2 帧、表有 32 页，扫描必然经历淘汰与重新装入；页数只多不少。
  EXPECT_GE(result.base_stats.physical_reads, env.layout.page_count());
  EXPECT_LT(result.base_stats.hit_rate(), 0.5);
}

TEST(TableScanner, ChecksumIsDeterministicAndSensitive) {
  const std::vector<std::byte> first(kRowSize - kRowHeaderSize, std::byte{0x01});

  EXPECT_EQ(ChecksumBytes(first), ChecksumBytes(first));

  std::vector<std::byte> second = first;
  second[0] = std::byte{0x02};
  EXPECT_NE(ChecksumBytes(first), ChecksumBytes(second));

  EXPECT_NE(ChecksumBytes(first), ChecksumBytes(std::span<const std::byte>(first).first(64)));
}

// 逐次扫描的增量只能算扫描自己的请求。池级计数器是所有线程共享的，并发 TP 的
// 页请求也落在里面；前后取差会把别人的读写算到扫描头上，明细因此失真。
TEST(TableScanner, PerScanStatsIgnoreConcurrentTraffic) {
  Env env(kTableRows, 8, 8, 0);  // 关预取，页数才好对
  FillDerivedRows(env);

  const ScanResult first = env.scanner.ScanBaseline(1);
  EXPECT_EQ(first.base_stats.logical_reads, env.layout.page_count());
  EXPECT_EQ(first.base_stats.physical_reads, env.layout.page_count());

  // 模拟并发 TP 的一次页请求：它只该进池级计数，不该进下一次扫描的明细。
  env.base.Unpin(env.base.Fix(0, AccessMode::kNormal));
  EXPECT_EQ(env.base.stats().logical_reads, env.layout.page_count() + 1);

  const ScanResult second = env.scanner.ScanBaseline(1);
  EXPECT_EQ(second.base_stats.logical_reads, env.layout.page_count());
}

// 扫描访问模式是 §7.2 的对照维度，必须真的传到池里。3 页的表、2 帧的池：
// 扫完之后 kNormal 留下的是最后两页，kScan 留下的是第一页与最后一页，
// 随后 Fix(0) 是否读盘就是判据。
TEST(TableScanner, ScanAccessModeReachesThePool) {
  const auto reads_after_fix = [](AccessMode mode) {
    Env env(96, 2, 8, 0);
    FillDerivedRows(env);
    TableScanner<BasePool, UndoPool> scanner(env.base, env.undo, env.layout, mode);
    scanner.ScanBaseline(1);
    env.base.Unpin(env.base.Fix(0, AccessMode::kNormal));
    return env.base_file.reads.load();
  };

  EXPECT_EQ(reads_after_fix(AccessMode::kNormal), 4u);  // 3 页 + 重新读回页 0
  EXPECT_EQ(reads_after_fix(AccessMode::kScan), 3u);    // 页 0 还在池里
}

}  // namespace

}  // namespace mvcc
