#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "scan/table_scanner.h"
#include "storage/page.h"
#include "storage/table_layout.h"
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
constexpr size_t kBaseFrames = 16;
constexpr size_t kUndoFrames = 8;
constexpr size_t kPrefetch = 8;
constexpr size_t kThreads = 4;
constexpr size_t kTxnsPerThread = 250;
constexpr size_t kHotRows = 64;
constexpr size_t kScans = 12;
constexpr uint32_t kInitial = 0xFFFFFFFFu;

using BasePool = BufferPool<DirectPageTable, LruReplacer>;
using UndoPool = BufferPool<FrameHashTable, LruReplacer>;
using Txn = Transaction<BasePool, UndoPool>;

struct Env {
  Env()
      : layout(kPageSize, kRowSize, kTableRows),
        base_file(layout.page_count(), layout.page_size(), test::PageFill::kZero),
        undo_file(kUndoPages, layout.page_size(), test::PageFill::kZero),
        base(layout, kBaseFrames, base_file, RowLockMode::kAllocate,
             DirectPageTable(layout.page_count()), LruReplacer(kBaseFrames), PoolHooks(), kPrefetch),
        undo(layout, kUndoFrames, undo_file, RowLockMode::kNone, FrameHashTable(kUndoFrames),
             LruReplacer(kUndoFrames)),
        appender(allocator, layout),
        timestamps(kThreads),
        scanner(base, undo, layout) {}

  TableLayout layout;
  test::MemoryPageFile base_file;
  test::MemoryPageFile undo_file;
  BasePool base;
  UndoPool undo;
  UndoPageAllocator allocator;
  UndoAppender appender;
  TimestampManager timestamps;
  TableScanner<BasePool, UndoPool> scanner;
};

struct CommitRecord {
  Timestamp ts;
  size_t row;
  uint32_t serial;
};

struct ScanSample {
  Timestamp read_ts;
  uint64_t checksum;
};

// 每个事务写一个由自身序号决定的值，oracle 可以脱离运行过程独立重算。
std::vector<std::byte> ValueFor(uint32_t serial, size_t size) {
  std::vector<std::byte> data(size);
  const uint64_t word = (uint64_t{serial} << 32) | 0xC0FFEEull;
  for (size_t offset = 0; offset + sizeof(word) <= size; offset += sizeof(word)) {
    std::memcpy(data.data() + offset, &word, sizeof(word));
  }
  return data;
}

uint64_t ExpectedChecksum(const std::vector<uint32_t>& state, size_t data_size) {
  uint64_t checksum = 0;
  for (const uint32_t serial : state) {
    if (serial == kInitial) continue;  // 初始行全零，折叠值恒为 0
    checksum ^= ChecksumBytes(ValueFor(serial, data_size));
  }
  return checksum;
}

TEST(MvccConcurrency, ConcurrentUpdatesAndScansMatchReplayOracle) {
  Env env;

  std::mutex log_mutex;
  std::vector<CommitRecord> log;
  std::atomic<size_t> conflicts{0};

  std::vector<std::thread> writers;
  writers.reserve(kThreads);
  for (size_t thread = 0; thread < kThreads; ++thread) {
    writers.emplace_back([&, thread] {
      TxnContext context(1, env.layout.data_size());
      UndoAppender appender(env.allocator, env.layout);
      uint64_t rng = 0x9E3779B97F4A7C15ull * (thread + 1);

      for (size_t index = 0; index < kTxnsPerThread; ++index) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        const size_t row = rng % kHotRows;
        const uint32_t serial = static_cast<uint32_t>(thread * kTxnsPerThread + index + 1);

        Txn txn(env.base, env.undo, env.layout, appender, env.timestamps, thread, context);
        txn.Begin();
        if (txn.Update(env.layout.RowIdAt(row), ValueFor(serial, env.layout.data_size())) !=
            UpdateOutcome::kApplied) {
          conflicts.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        txn.Commit();

        const std::lock_guard<std::mutex> guard(log_mutex);
        log.push_back(CommitRecord{txn.commit_ts(), row, serial});
      }
    });
  }

  std::vector<ScanSample> samples;
  std::thread scanner([&] {
    for (size_t index = 0; index < kScans; ++index) {
      const Timestamp read_ts = env.timestamps.ReadTimestamp();
      const ScanResult result = env.scanner.ScanBaseline(read_ts);
      samples.push_back(ScanSample{read_ts, result.checksum});
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  for (std::thread& writer : writers) writer.join();
  scanner.join();

  EXPECT_GT(log.size(), 0u);
  EXPECT_GT(conflicts.load(), 0u) << "热点行负载必须真的发生写冲突";

  // 终态扫描：此时没有进行中的提交，read_ts 覆盖全部记录。
  const Timestamp final_ts = env.timestamps.ReadTimestamp();
  const ScanResult terminal = env.scanner.ScanBaseline(final_ts);

  std::sort(log.begin(), log.end(),
            [](const CommitRecord& left, const CommitRecord& right) { return left.ts < right.ts; });
  std::sort(samples.begin(), samples.end(),
            [](const ScanSample& left, const ScanSample& right) {
              return left.read_ts < right.read_ts;
            });

  // 按 commit_ts 升序回放：read_ts = R 的快照可见的正是"ts < R 的提交全部应用后"
  // 的状态。若某个已提交版本被并发事务覆盖掉（链头被改、旧版本丢失），这里会
  // 第一次比对就失败。
  std::vector<uint32_t> state(kTableRows, kInitial);
  size_t applied = 0;
  for (const ScanSample& sample : samples) {
    while (applied < log.size() && log[applied].ts < sample.read_ts) {
      state[log[applied].row] = log[applied].serial;
      ++applied;
    }
    EXPECT_EQ(sample.checksum, ExpectedChecksum(state, env.layout.data_size()))
        << "scan at read_ts=" << sample.read_ts;
  }

  while (applied < log.size()) {
    state[log[applied].row] = log[applied].serial;
    ++applied;
  }
  EXPECT_EQ(terminal.checksum, ExpectedChecksum(state, env.layout.data_size()));
  EXPECT_EQ(terminal.rows, env.layout.row_count());

  // 每个已提交版本都必须能被"紧随其后的快照"读到：read_ts = commit_ts + 1 时
  // 可见的正是这一版。若某个提交被并发事务覆盖（链头被改、旧版本从链上消失），
  // 这里会直接读到更老的值。这条检查不依赖采样时机。
  for (const CommitRecord& record : log) {
    std::vector<std::byte> visible(env.layout.data_size());
    ReadVisible(env.base, env.undo, env.layout, env.layout.RowIdAt(record.row),
                Timestamp{record.ts + 1}, AccessMode::kNormal, visible);
    EXPECT_EQ(ChecksumBytes(visible), ChecksumBytes(ValueFor(record.serial, env.layout.data_size())))
        << "row " << record.row << " commit_ts " << record.ts;
  }
}

}  // namespace
}  // namespace mvcc
