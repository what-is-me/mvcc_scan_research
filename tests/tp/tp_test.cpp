#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "scan/table_scanner.h"
#include "storage/table_layout.h"
#include "txn/timestamp_manager.h"
#include "txn/undo_log.h"
#include "tp/tp_runner.h"

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
constexpr size_t kUndoPages = 2400;
constexpr size_t kPrefetch = 8;
constexpr uint64_t kInitial = 0;

using BasePool = BufferPool<DirectPageTable, LruReplacer>;
using UndoPool = BufferPool<FrameHashTable, LruReplacer>;

struct Env {
  explicit Env(size_t undo_pages = kUndoPages)
      : layout(kPageSize, kRowSize, kTableRows),
        base_file(layout.page_count(), layout.page_size(), test::PageFill::kZero),
        undo_file(undo_pages, layout.page_size(), test::PageFill::kZero),
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
    config.key_dist = KeyDist::kZipf08;
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
    config.key_dist = KeyDist::kZipf08;
    config.mapping = mapping;
    RunTp(env.base, env.undo, env.layout, env.allocator, env.timestamps, config);
    return env.base.stats().physical_reads;
  };

  const uint64_t head_reads = run(KeyMapping::kHead);
  const uint64_t scattered_reads = run(KeyMapping::kScattered);

  EXPECT_GT(head_reads, 0u);
  EXPECT_LT(head_reads, scattered_reads);
}

struct ScanSample {
  Timestamp read_ts = 0;
  uint64_t checksum = 0;
};

// §12 的中途扫描校验：TP 进行中反复取快照扫描，每次都与"回放
// commit_ts < read_ts 的提交"后的期望比对。终态 oracle 只能保住最后一步，
// 读到未提交版本、链头跳过已提交版本这类错误必须靠运行期采样暴露。
TEST(TpWorkload, MidRunScansMatchReplayOracle) {
  // 40000 次更新需要约 2500 页 undo，给到 5000。
  Env env(5000);

  TpConfig config;
  config.threads = 2;
  config.rows_per_txn = 1;
  config.txns_per_thread = 40000;
  config.key_dist = KeyDist::kZipf08;
  config.mapping = KeyMapping::kScattered;

  const KeyMapper mapper(env.layout.row_count(), config.mapping, config.seed);
  std::vector<TpOutcome> partial(config.threads);

  std::atomic<bool> stop{false};
  std::vector<ScanSample> samples;

  std::thread scanner([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const Timestamp read_ts = env.timestamps.ReadTimestamp();
      const ScanResult result = env.scanner.ScanBaseline(read_ts);
      samples.push_back(ScanSample{read_ts, result.checksum});
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::vector<std::thread> workers;
  workers.reserve(config.threads);
  for (size_t thread = 0; thread < config.threads; ++thread) {
    workers.emplace_back([&, thread] {
      RunTpWorker(env.base, env.undo, env.layout, env.allocator, env.timestamps, mapper,
                  config, thread, nullptr, partial[thread]);
    });
  }
  for (std::thread& worker : workers) worker.join();
  stop.store(true, std::memory_order_release);
  scanner.join();

  const TpOutcome outcome = MergeTpOutcomes(partial);
  ASSERT_GT(outcome.committed, 0u);
  ASSERT_GT(samples.size(), 3u);

  // read_ts 单调不减：counter 单调，新取号的 ts 必 >= 上一个 counter，
  // 释放只会把最老的进行中项移出集合。采样顺序即回放顺序。
  for (size_t index = 1; index < samples.size(); ++index) {
    ASSERT_LE(samples[index - 1].read_ts, samples[index].read_ts)
        << "ReadTimestamp 返回了回退的快照";
  }

  std::vector<uint64_t> state(env.layout.row_count(), kInitial);
  size_t applied = 0;
  for (const ScanSample& sample : samples) {
    while (applied < outcome.commits.size() &&
           outcome.commits[applied].commit_ts < sample.read_ts) {
      state[env.layout.LinearRow(outcome.commits[applied].row)] = outcome.commits[applied].serial;
      ++applied;
    }
    EXPECT_EQ(sample.checksum, ExpectedChecksum(state, env.layout.data_size()))
        << "scan at read_ts=" << sample.read_ts;
  }
}

// 同一 read_ts 下四种策略必须读到同一份快照。运行中只比 checksum：两次扫描
// 之间链还在增长，新版本的 ts 都 >= read_ts，回溯会多经过它们，走链行数与
// 跳数都会变，但可见版本集合不变。TP 停下来之后链不再增长，那时才逐字段比。
TEST(TpWorkload, AllScanPoliciesAgreeUnderConcurrentTp) {
  Env env(5000);

  TpConfig config;
  config.threads = 3;
  config.rows_per_txn = 4;
  config.txns_per_thread = 8000;
  config.key_dist = KeyDist::kZipf08;
  config.mapping = KeyMapping::kScattered;

  const KeyMapper mapper(env.layout.row_count(), config.mapping, config.seed);
  std::vector<TpOutcome> partial(config.threads);
  std::atomic<size_t> running{config.threads};

  std::vector<std::thread> workers;
  workers.reserve(config.threads);
  for (size_t thread = 0; thread < config.threads; ++thread) {
    workers.emplace_back([&, thread] {
      RunTpWorker(env.base, env.undo, env.layout, env.allocator, env.timestamps, mapper, config,
                  thread, nullptr, partial[thread]);
      running.fetch_sub(1, std::memory_order_release);
    });
  }

  const ScanPolicy policies[] = {ScanPolicy::kBaseline, ScanPolicy::kHotFirst,
                                 ScanPolicy::kDeferChain, ScanPolicy::kDeferPaged};
  size_t live_samples = 0;
  for (int sample = 0; sample < 4; ++sample) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (running.load(std::memory_order_acquire) == 0) break;

    const Timestamp read_ts = env.timestamps.ReadTimestamp();
    const ScanResult baseline = env.scanner.Scan(read_ts, ScanPolicy::kBaseline);
    for (const ScanPolicy policy : policies) {
      const ScanResult result = env.scanner.Scan(read_ts, policy);
      EXPECT_EQ(result.rows, baseline.rows);
      EXPECT_EQ(result.checksum, baseline.checksum)
          << "policy=" << static_cast<int>(policy) << " read_ts=" << read_ts;
    }
    ++live_samples;
  }
  for (std::thread& worker : workers) worker.join();

  ASSERT_GT(live_samples, 0u) << "TP 在第一次采样前就结束了，用例没测到并发";
  ASSERT_GT(MergeTpOutcomes(partial).committed, 0u);

  const Timestamp settled = env.timestamps.ReadTimestamp();
  const ScanResult baseline = env.scanner.Scan(settled, ScanPolicy::kBaseline);
  for (const ScanPolicy policy : policies) {
    const ScanResult result = env.scanner.Scan(settled, policy);
    EXPECT_EQ(result.rows, baseline.rows);
    EXPECT_EQ(result.chain_rows, baseline.chain_rows);
    EXPECT_EQ(result.chain_hops, baseline.chain_hops);
    EXPECT_EQ(result.checksum, baseline.checksum);
  }
}

// 实验按"时长"而不是事务数结束：stop 旗标在事务之间检查。record_log=false
// 时不攒提交日志（benchmark 用不到），延迟直方图始终记录。
// 唯一按时长满跑的用例：内存文件无 I/O，双线程实测约 80 万事务/秒，100ms
// 就能烧掉 ~2500 页 undo（页号全局递增、无上界，容量保证属配置校验层）。
// undo 页数必须按这个速率配足余量，否则写回越界——MemoryPageFile 的边界
// 检查会把这类配错明确报出来，而不是让进程偶发踩堆崩溃。
TEST(TpWorkload, WorkerStopsOnFlagAndSkipsCommitLog) {
  Env env(4096);  // 4096 页 × 32 条/页 = 13 万条记录，60ms 满跑上界（4.8 万条）的 2.7 倍

  TpConfig config;
  config.threads = 2;
  config.rows_per_txn = 1;
  config.txns_per_thread = 1'000'000'000ull;
  config.key_dist = KeyDist::kUniform;
  config.record_log = false;
  const KeyMapper mapper(env.layout.row_count(), config.mapping, config.seed);

  std::atomic<bool> stop{false};
  std::vector<TpOutcome> partial(config.threads);
  std::vector<std::thread> workers;
  for (size_t thread = 0; thread < config.threads; ++thread) {
    workers.emplace_back([&, thread] {
      RunTpWorker(env.base, env.undo, env.layout, env.allocator, env.timestamps, mapper, config,
                  thread, &stop, partial[thread]);
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  stop.store(true, std::memory_order_release);
  for (std::thread& worker : workers) worker.join();

  const TpOutcome merged = MergeTpOutcomes(partial);
  EXPECT_GT(merged.committed, 0u);
  EXPECT_EQ(merged.commits.size(), 0u);           // record_log=false
  EXPECT_GT(merged.latency.count(), 0u);          // 延迟始终记录
}

// 键分布通过 TpConfig::key_dist 进入负载层。调度决定不了采样序列（种子固定），
// 冲突只让个别事务缺席，所以"头部 1% 行吃掉多少比例的更新"是稳定的接线信号：
// head 映射下 zipf-0.8 约 20%~28%，均匀分布约 1%，差距五倍以上。
TEST(TpWorkload, KeyDistWiringConcentratesUpdatesOnTheHeadRows) {
  const auto run = [](KeyDist dist) {
    Env env(2400);
    TpConfig config;
    config.threads = kThreads;
    config.rows_per_txn = 1;
    config.txns_per_thread = kTxnsPerThread;
    config.key_dist = dist;
    config.mapping = KeyMapping::kHead;
    return RunTp(env.base, env.undo, env.layout, env.allocator, env.timestamps, config);
  };

  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  const auto hot_updates = [&layout](const TpOutcome& outcome) {
    const size_t cutoff = layout.row_count() / 100;  // 前 1% 的行
    size_t hot = 0;
    for (const CommitEntry& entry : outcome.commits) {
      if (layout.LinearRow(entry.row) < cutoff) ++hot;
    }
    return hot;
  };

  const size_t uniform_hot = hot_updates(run(KeyDist::kUniform));
  const size_t zipf_hot = hot_updates(run(KeyDist::kZipf08));
  const size_t attempts = kThreads * kTxnsPerThread;

  EXPECT_LT(uniform_hot, attempts / 50);   // 期望约 1%
  EXPECT_GT(zipf_hot, attempts / 10);      // 期望约 20%~28%
  EXPECT_GT(zipf_hot, uniform_hot * 5);
}

}  // namespace
}  // namespace mvcc
