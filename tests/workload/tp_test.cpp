#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "scan/table_scanner.h"
#include "storage/table_layout.h"
#include "txn/timestamp_manager.h"
#include "txn/undo_log.h"
#include "workload/tp_runner.h"

#include "../buffer/memory_page_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kTableRows = 10000;
constexpr size_t kThreads = 4;
constexpr size_t kTxnsPerThread = 500;
constexpr size_t kBaseFrames = 128;
constexpr size_t kUndoFrames = 128;
constexpr size_t kUndoPages = 1200;
constexpr size_t kPrefetch = 8;
constexpr uint64_t kInitial = 0;

using BasePool = BufferPool<DirectPageTable, LruReplacer>;
using UndoPool = BufferPool<FrameHashTable, LruReplacer>;

struct Env {
  Env()
      : layout(kPageSize, kRowSize, kTableRows),
        base_file(layout.page_count(), layout.page_size(), test::PageFill::kZero),
        undo_file(kUndoPages, layout.page_size(), test::PageFill::kZero),
        base(layout, kBaseFrames, base_file, RowLockMode::kAllocate,
             DirectPageTable(layout.page_count()), LruReplacer(kBaseFrames), PoolHooks(), kPrefetch),
        undo(layout, kUndoFrames, undo_file, RowLockMode::kNone, FrameHashTable(kUndoFrames),
             LruReplacer(kUndoFrames)),
        timestamps(kThreads),
        scanner(base, undo, layout) {}

  TableLayout layout;
  test::MemoryPageFile base_file;
  test::MemoryPageFile undo_file;
  BasePool base;
  UndoPool undo;
  UndoPageAllocator allocator;
  TimestampManager timestamps;
  TableScanner<BasePool, UndoPool> scanner;
};

uint64_t ExpectedChecksum(const std::vector<uint64_t>& state, size_t data_size) {
  std::vector<std::byte> value(data_size);
  uint64_t checksum = 0;
  for (const uint64_t serial : state) {
    if (serial == kInitial) continue;  // 初始行全零，折叠值恒为 0
    FillValue(serial, value);
    checksum ^= ChecksumBytes(value);
  }
  return checksum;
}

TEST(TpWorkload, TerminalStateMatchesReplayOracle) {
  for (const size_t rows_per_txn : {size_t{1}, size_t{4}, size_t{16}}) {
    Env env;

    TpConfig config;
    config.threads = kThreads;
    config.rows_per_txn = rows_per_txn;
    config.txns_per_thread = kTxnsPerThread;
    config.zipf_theta = 0.8;
    config.mapping = KeyMapping::kScattered;

    const TpOutcome outcome =
        RunTp(env.base, env.undo, env.layout, env.allocator, env.timestamps, config);

    ASSERT_GT(outcome.committed, 0u) << "rows_per_txn=" << rows_per_txn;
    // 16 行事务在 Zipf 热点上冲突率超过一半，只要求"没有整体空跑"。
    EXPECT_GT(outcome.committed, config.threads * config.txns_per_thread / 10)
        << "rows_per_txn=" << rows_per_txn;
    ASSERT_EQ(outcome.commits.size(), outcome.committed * rows_per_txn)
        << "rows_per_txn=" << rows_per_txn;
    EXPECT_GT(outcome.conflicts, 0u) << "热点负载必须真的发生写冲突";

    // 按 commit_ts 升序回放：最后一次写入即终态。
    std::vector<uint64_t> state(env.layout.row_count(), kInitial);
    Timestamp previous = 0;
    for (const CommitEntry& entry : outcome.commits) {
      ASSERT_GE(entry.commit_ts, previous) << "提交日志未按 commit_ts 排序";
      previous = entry.commit_ts;
      state[env.layout.LinearRow(entry.row)] = entry.serial;
    }

    const ScanResult terminal = env.scanner.ScanBaseline(env.timestamps.ReadTimestamp());
    EXPECT_EQ(terminal.checksum, ExpectedChecksum(state, env.layout.data_size()))
        << "rows_per_txn=" << rows_per_txn;
    EXPECT_EQ(terminal.rows, env.layout.row_count());
  }
}

// 排名到行号是双射，两种映射的行级冲突率统计上相同；差别在页级分布：
// head 把前 1% 压进几页，这些页常驻，物理读明显更少。§11 默认 scattered
// 是为了让热点页由页内热点行密度自然涌现，而不是被映射人为压缩。
TEST(TpWorkload, HeadMappingTouchesFewerPages) {
  const auto run = [](KeyMapping mapping) {
    Env env;
    TpConfig config;
    config.threads = kThreads;
    config.rows_per_txn = 1;
    config.txns_per_thread = kTxnsPerThread;
    config.zipf_theta = 0.8;
    config.mapping = mapping;
    RunTp(env.base, env.undo, env.layout, env.allocator, env.timestamps, config);
    return env.base.stats().physical_reads;
  };

  const uint64_t head_reads = run(KeyMapping::kHead);
  const uint64_t scattered_reads = run(KeyMapping::kScattered);

  EXPECT_GT(head_reads, 0u);
  EXPECT_LT(head_reads, scattered_reads);
}

}  // namespace
}  // namespace mvcc
