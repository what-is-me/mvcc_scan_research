#include <algorithm>
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

using BasePool = BufferPool<FrameHashTable, LruReplacer>;
using UndoPool = BufferPool<FrameHashTable, LruReplacer>;
using Txn = Transaction<BasePool, UndoPool>;

struct Env {
  explicit Env(size_t table_rows = kTableRows, size_t base_frames = 8, size_t undo_frames = 8,
               size_t prefetch = 8)
      : layout(kPageSize, kRowSize, table_rows),
        base_file(layout.page_count(), layout.page_size(), test::PageFill::kZero),
        undo_file(kUndoPages, layout.page_size(), test::PageFill::kZero),
        base(layout, base_frames, base_file, RowLockMode::kAllocate,
             FrameHashTable(base_frames), LruReplacer(base_frames), PoolHooks(), prefetch),
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

// 扫描策略之间必须逐字节可比：同一 read_ts 下四种策略读到的可见版本集合
// 完全相同，差别只能体现在页的访问次序上。
void ExpectPoliciesAgreeWithBaseline() {
  Env env;
  FillDerivedRows(env);

  const std::vector<std::byte> one(env.layout.data_size(), std::byte{0x11});
  const std::vector<std::byte> two(env.layout.data_size(), std::byte{0x22});
  const std::vector<std::byte> three(env.layout.data_size(), std::byte{0x33});

  // 快照先取，之后的提交全部落在快照之后——包括链长 3 的那一行。
  const Timestamp read_ts = env.timestamps.ReadTimestamp();
  UpdateRow(env, RowId::Make(0, 1), one, /*commit=*/true);
  UpdateRow(env, RowId::Make(9, 5), two, /*commit=*/true);
  UpdateRow(env, RowId::Make(9, 5), three, /*commit=*/true);
  UpdateRow(env, RowId::Make(9, 5), one, /*commit=*/true);

  // 再加一行被未提交事务置锁：基表版本被锁位挡住，同样要走链。
  Txn txn(env.base, env.undo, env.layout, env.appender, env.timestamps, 0, env.context);
  txn.Begin();
  ASSERT_EQ(txn.Update(RowId::Make(20, 7), two), UpdateOutcome::kApplied);

  const ScanResult baseline = env.scanner.Scan(read_ts, ScanPolicy::kBaseline);
  // 三行都落在快照之后：一行更新一次、一行更新三次、一行未提交。
  EXPECT_EQ(baseline.chain_rows, 3u);
  EXPECT_EQ(baseline.chain_hops, 5u);
  EXPECT_EQ(baseline.checksum, ExpectedChecksum(env.layout));

  const ScanPolicy others[] = {ScanPolicy::kHotFirst, ScanPolicy::kHotFirstBypass,
                               ScanPolicy::kDeferChain, ScanPolicy::kDeferPaged};
  for (const ScanPolicy policy : others) {
    const ScanResult result = env.scanner.Scan(read_ts, policy);
    EXPECT_EQ(result.rows, baseline.rows);
    EXPECT_EQ(result.chain_rows, baseline.chain_rows);
    EXPECT_EQ(result.chain_hops, baseline.chain_hops);
    EXPECT_EQ(result.checksum, baseline.checksum);
  }

  txn.Abort();
}

TEST(TableScanner, HotFirstAgreesWithBaseline) {
  ExpectPoliciesAgreeWithBaseline();
}

// defer 的动机是把 undo 访问整体挪出主扫描。主扫描必须一行都不碰 undo 池，
// 否则"顺带读到了"的页会污染两阶段的计时。
TEST(TableScanner, DeferMainScanLeavesUndoUntouched) {
  Env env;
  FillDerivedRows(env);

  const Timestamp read_ts = env.timestamps.ReadTimestamp();
  const std::vector<std::byte> updated(env.layout.data_size(), std::byte{0x77});
  UpdateRow(env, RowId::Make(1, 2), updated, /*commit=*/true);

  DeferredScan scan = env.scanner.ScanDeferMain(read_ts);

  EXPECT_EQ(scan.result.rows, env.layout.row_count());
  EXPECT_EQ(scan.result.chain_rows, 1u);
  EXPECT_EQ(scan.result.chain_hops, 0u);
  EXPECT_EQ(scan.pending.size(), 1u);
  EXPECT_EQ(scan.result.undo_stats.logical_reads, 0u);
  EXPECT_EQ(scan.result.undo_stats.physical_reads, 0u);
  // 主扫描的折叠只含基表可见版本：走链那一行留给第二阶段，此刻还没算进去。
  const RowId deferred = RowId::Make(1, 2);
  const uint64_t original = ExpectedChecksum(env.layout);
  EXPECT_EQ(scan.result.checksum,
            original ^ ChecksumBytes(RowData(env.layout.LinearRow(deferred),
                                             env.layout.data_size())));

  env.scanner.ResolveDeferredChain(scan, read_ts);

  EXPECT_TRUE(scan.pending.empty());
  EXPECT_EQ(scan.result.chain_hops, 1u);
  EXPECT_EQ(scan.result.checksum, original);
}

// 页式的收益就是 undo 页的重复加载次数：同一个 undo 页上的多条链，链式每条
// 各 Fix 一次，页式只装一次就在页内解析完。
TEST(TableScanner, DeferPagedLoadsEachUndoPageOnce) {
  const auto undo_reads = [](ScanPolicy policy) {
    Env env(/*table_rows=*/32, 8, 8, /*prefetch=*/0);  // 单页基表
    FillDerivedRows(env);

    const Timestamp read_ts = env.timestamps.ReadTimestamp();
    for (size_t slot = 0; slot < 8; ++slot) {
      const std::vector<std::byte> data(env.layout.data_size(), static_cast<std::byte>(slot + 1));
      UpdateRow(env, RowId::Make(0, slot), data, /*commit=*/true);
    }

    const ScanResult result = env.scanner.Scan(read_ts, policy);
    EXPECT_EQ(result.chain_rows, 8u);
    EXPECT_EQ(result.chain_hops, 8u);
    return result.undo_stats.logical_reads;
  };

  EXPECT_EQ(undo_reads(ScanPolicy::kDeferChain), 8u);
  EXPECT_EQ(undo_reads(ScanPolicy::kDeferPaged), 1u);
}

// 链跨 undo 页时页式要多轮迭代：每轮每个页只装一次，跨页的 prev 进下一轮。
TEST(TableScanner, DeferPagedFollowsChainsAcrossPages) {
  Env env(32, 8, 8, 0);
  FillDerivedRows(env);

  // 槽位 0 到 31 是同一页，第 33 条记录才落到第二页；每行更新一次只能造出
  // 单跳链，所以改成一行的版本在页边界附近跨过去。
  const Timestamp read_ts = env.timestamps.ReadTimestamp();
  const RowId row = RowId::Make(0, 3);
  for (uint8_t seed = 1; seed <= 40; ++seed) {
    const std::vector<std::byte> data(env.layout.data_size(), static_cast<std::byte>(seed));
    UpdateRow(env, row, data, /*commit=*/true);
  }

  const ScanResult paged = env.scanner.Scan(read_ts, ScanPolicy::kDeferPaged);
  const ScanResult baseline = env.scanner.Scan(read_ts, ScanPolicy::kBaseline);

  EXPECT_EQ(paged.chain_rows, 1u);
  EXPECT_EQ(paged.chain_hops, baseline.chain_hops);
  EXPECT_EQ(paged.chain_hops, 40u);
  EXPECT_EQ(paged.checksum, baseline.checksum);
  // 40 条记录跨两页：第一轮装页 1 解析 8 条，跨页后第二轮装页 0 走完。
  EXPECT_EQ(paged.undo_stats.logical_reads, 2u);
  EXPECT_EQ(baseline.undo_stats.logical_reads, 40u);
}

// hotfirst 的收益必须体现为物理读：热点页在快照里，先扫它们只花命中；baseline
// 从页 0 顺序推进，冷页会把热点页挤出去，扫到热点页时还得重新读回来。
//
// 这个用例把扫描访问模式关掉：kScan 下新装入的页本就优先淘汰，扫描自己不会
// 挤占常驻页，两种次序的 I/O 差异被这个模式掩盖。hotfirst 的收益只在
// kNormal 下可见——两个模式的对照本身就是实验维度（§7.2）。
TEST(TableScanner, HotFirstSavesTheReadsBaselinePaysFor) {
  const auto run = [](ScanPolicy policy) {
    Env env(/*table_rows=*/128, /*base_frames=*/2, /*undo_frames=*/8, /*prefetch=*/0);
    FillDerivedRows(env);
    EXPECT_EQ(env.layout.page_count(), 4u);

    // 池里先装 2、3 两页，它们就是扫描开始瞬间的热点集合。
    env.base.Unpin(env.base.Fix(3, AccessMode::kNormal));
    env.base.Unpin(env.base.Fix(2, AccessMode::kNormal));

    TableScanner<BasePool, UndoPool> scanner(env.base, env.undo, env.layout, AccessMode::kNormal);
    const ScanResult result = scanner.Scan(1, policy);
    return result.base_stats.physical_reads;
  };

  EXPECT_EQ(run(ScanPolicy::kHotFirst), 2u);   // 扫 2、3 命中；再读 0、1
  EXPECT_EQ(run(ScanPolicy::kBaseline), 4u);   // 扫 0、1 时把 2、3 挤掉，又读回来
}

// hotfirst_bypass：冷段查池直读（§7.2 / §10）。热点页 2、3 预先装进池（即
// 快照里的热点集合），冷段 = {0, 1} 两页均未驻留——直读合并成一次设备读，
// 且不把冷页装进池（扫描后驻留集合不变）。计数口径：直读页计入逻辑读与
// 逻辑未命中，物理读按页、读调用按次。
TEST(TableScanner, HotFirstBypassReadsColdPagesDirectly) {
  Env env(/*table_rows=*/128, /*base_frames=*/4, /*undo_frames=*/8, /*prefetch=*/4);
  FillDerivedRows(env);
  EXPECT_EQ(env.layout.page_count(), 4u);

  env.base.Unpin(env.base.Fix(3, AccessMode::kNormal));
  env.base.Unpin(env.base.Fix(2, AccessMode::kNormal));

  TableScanner<BasePool, UndoPool> scanner(env.base, env.undo, env.layout);
  const ScanResult result =
      scanner.Scan(env.timestamps.ReadTimestamp(), ScanPolicy::kHotFirstBypass);

  EXPECT_EQ(result.rows, env.layout.row_count());
  EXPECT_EQ(result.checksum, ExpectedChecksum(env.layout));

  // 逻辑读 = 全部 4 页（热点 2 页池命中 + 冷段 2 页直读）；物理读只有冷段，
  // 且连续子段合并成一次设备读。
  EXPECT_EQ(result.base_stats.logical_reads, 4u);
  EXPECT_EQ(result.base_stats.logical_misses, 2u);
  EXPECT_EQ(result.base_stats.physical_reads, 2u);
  EXPECT_EQ(result.base_stats.physical_read_ops, 1u);
  EXPECT_DOUBLE_EQ(result.base_stats.hit_rate(), 0.5);

  // 直读不经池帧：扫描后驻留集合仍是热点 {2, 3}，冷页没有进池。
  std::vector<PageId> resident;
  env.base.CollectResidentPages(resident);
  std::sort(resident.begin(), resident.end());
  EXPECT_EQ(resident, (std::vector<PageId>{2, 3}));
}

// O1 端到端：提交过的页被淘汰写回后，直读必须看到已提交的新版本。IN4
// （写回先于条目清除）是"页不在池 ⇒ 磁盘镜像 = 最新已提交状态"的来源。
TEST(TableScanner, HotFirstBypassSeesCommittedDataAfterEviction) {
  Env env(/*table_rows=*/128, /*base_frames=*/2, /*undo_frames=*/8, /*prefetch=*/4);
  FillDerivedRows(env);

  const RowId row = RowId::Make(0, 5);
  const size_t linear = row.page() * env.layout.rows_per_page() + row.slot();
  const std::vector<std::byte> updated(env.layout.data_size(), std::byte{0x66});
  UpdateRow(env, row, updated, /*commit=*/true);
  // 快照必须取在提交之后：取号与读快照都用同一个计数器，先取的 read_ts 会
  // 与首次提交的 commit_ts 相同，按 §8 语义（ts ≥ read_ts 不可见）走链返回
  // 旧版本——那是另一条用例的职责。
  const Timestamp after = env.timestamps.ReadTimestamp();

  // 把脏页挤出池：再装 1、2 两页，2 帧的池必然淘汰页 0（LRU 最旧）并写回。
  env.base.Unpin(env.base.Fix(1, AccessMode::kNormal));
  env.base.Unpin(env.base.Fix(2, AccessMode::kNormal));
  ASSERT_GE(env.base.stats().physical_writes, 1u);

  TableScanner<BasePool, UndoPool> scanner(env.base, env.undo, env.layout);
  const ScanResult result = scanner.Scan(after, ScanPolicy::kHotFirstBypass);

  EXPECT_EQ(result.checksum, ExpectedChecksum(env.layout) ^
                                 ChecksumBytes(RowData(linear, env.layout.data_size())) ^
                                 ChecksumBytes(updated));
}

// 直读页在快照之后又有提交：盘上版本 ts ≥ read_ts，判定失败沿盘上链头走链，
// 返回快照可见的旧版本。链头与数据同页写回（I5 + IN4），undo 记录由 undo 池
// 服务——bypass 只作用于基表页，版本解析路径不变。
TEST(TableScanner, HotFirstBypassWalksTheChainWhenDiskIsNewer) {
  Env env(/*table_rows=*/128, /*base_frames=*/2, /*undo_frames=*/8, /*prefetch=*/4);
  FillDerivedRows(env);

  const RowId row = RowId::Make(0, 5);
  const Timestamp before = env.timestamps.ReadTimestamp();
  const std::vector<std::byte> updated(env.layout.data_size(), std::byte{0x66});
  UpdateRow(env, row, updated, /*commit=*/true);

  // 提交后把脏页挤出池：盘上版本已经是新提交（ts ≥ read_ts）。
  env.base.Unpin(env.base.Fix(1, AccessMode::kNormal));
  env.base.Unpin(env.base.Fix(2, AccessMode::kNormal));
  ASSERT_GE(env.base.stats().physical_writes, 1u);

  TableScanner<BasePool, UndoPool> scanner(env.base, env.undo, env.layout);
  const ScanResult result = scanner.Scan(before, ScanPolicy::kHotFirstBypass);

  // 快照在提交之前：直读拿到的新版本不可见，走链返回旧值。
  EXPECT_EQ(result.chain_rows, 1u);
  EXPECT_EQ(result.chain_hops, 1u);
  EXPECT_EQ(result.checksum, ExpectedChecksum(env.layout));
}

// I2 的窗口：读者先看到旧链头，写者随后追加记录并把链头换掉。判定失败时
// 必须重读链头——从旧值回溯会跳过写者刚追加的那条记录，而它恰好就是快照
// 需要的版本。这里用真实的 Row 复现这个交错，不依赖并发时序。
TEST(TableScanner, RereadsTheChainHeadAfterTheWriterSwapsIt) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  std::vector<std::byte> bytes(layout.page_size());
  const Page page(std::span<std::byte>(bytes), layout);
  Row row = page.RowAt(0);

  const RowId stale = RowId::Make(1, 1);
  const RowId fresh = RowId::Make(2, 2);
  row.set_version(VersionStamp::Committed(7));
  row.set_undo_row_id(stale);

  // 读者手里先拿着旧链头，写者随后换链头——真实交错里这两步分别发生在
  // "首次读到 undo_row_id"与"重读"之间。
  UndoRowId cursor = stale;
  row.set_undo_row_id(fresh);

  EXPECT_FALSE(scan_detail::CheckBaseVersion(row, /*read_ts=*/7, cursor));
  EXPECT_EQ(cursor, fresh);
}

TEST(TableScanner, LeavesTheCursorAloneWhenTheBaseVersionIsVisible) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  std::vector<std::byte> bytes(layout.page_size());
  const Page page(std::span<std::byte>(bytes), layout);
  Row row = page.RowAt(0);

  const RowId stale = RowId::Make(1, 1);
  row.set_version(VersionStamp::Committed(7));
  row.set_undo_row_id(stale);

  UndoRowId cursor = RowId::Null();
  EXPECT_TRUE(scan_detail::CheckBaseVersion(row, /*read_ts=*/8, cursor));
  EXPECT_TRUE(cursor.is_null());
}

// 两段耗时是 details[] 的两个字段，必须真的有人产出：baseline/hotfirst 只有
// 主扫描，resolve_ms 恒为 0；defer 的两段分别累计。
TEST(TableScanner, ReportsTwoStageTimings) {
  Env env;
  FillDerivedRows(env);

  const Timestamp read_ts = env.timestamps.ReadTimestamp();
  const std::vector<std::byte> updated(env.layout.data_size(), std::byte{0x44});
  UpdateRow(env, RowId::Make(2, 3), updated, /*commit=*/true);

  const ScanResult baseline = env.scanner.Scan(read_ts, ScanPolicy::kBaseline);
  EXPECT_GT(baseline.main_scan_ms, 0.0);
  EXPECT_EQ(baseline.resolve_ms, 0.0);

  DeferredScan deferred = env.scanner.ScanDeferMain(read_ts);
  EXPECT_EQ(deferred.result.chain_rows, 1u);
  EXPECT_GT(deferred.result.main_scan_ms, 0.0);
  EXPECT_EQ(deferred.result.resolve_ms, 0.0);

  env.scanner.ResolveDeferredChain(deferred, read_ts);
  EXPECT_GT(deferred.result.resolve_ms, 0.0);
}

// 快照里出现重复页号时，冷段归并不能漏页：漏掉的话该页既留在热段又被
// 冷段扫一遍，XOR 折叠会把它静默抹掉。
TEST(TableScanner, ColdSegmentSkipsDuplicatesInTheSnapshot) {
  const std::vector<PageId> hot = {2, 2, 2, 3};
  std::vector<PageId> cold;
  scan_detail::BuildColdPages(hot, 6, cold);
  EXPECT_EQ(cold, (std::vector<PageId>{0, 1, 4, 5}));
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
