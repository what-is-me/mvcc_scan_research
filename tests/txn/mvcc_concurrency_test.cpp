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

// §12 竞态放大的闸门：第 threshold 次钩子触发时挂起，直到主线程放行。
// WaitReached 带时限——钩子被改坏（不再触发）时测试失败而不是挂死。
class Gate {
 public:
  explicit Gate(int threshold) : threshold_(threshold) {}

  void Fire() {
    if (fired_.fetch_add(1, std::memory_order_acq_rel) + 1 < threshold_) return;
    reached_.store(true, std::memory_order_release);
    while (!release_.load(std::memory_order_acquire)) std::this_thread::yield();
  }

  bool WaitReached() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!reached_.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::yield();
    }
    return true;
  }

  void Release() { release_.store(true, std::memory_order_release); }

 private:
  int threshold_;
  std::atomic<int> fired_{0};
  std::atomic<bool> reached_{false};
  std::atomic<bool> release_{false};
};

void FireGate(void* context, TransactionHookPoint) { static_cast<Gate*>(context)->Fire(); }

// first-updater-wins 必须由这里显式覆盖：靠调度去撞冲突并不可靠（单事务只有
// 几微秒，四个线程可能整段跑在同一个时间片里完全串行）。两个事务在同一行上
// 前后脚开始，read_ts 相同，先到的置锁，后到的必须被拒。
TEST(MvccConcurrency, UpdateRejectsASecondWriterOfTheSameRow) {
  Env env;

  TxnContext context_a(1, env.layout.data_size());
  TxnContext context_b(1, env.layout.data_size());
  UndoAppender appender_a(env.allocator, env.layout);
  UndoAppender appender_b(env.allocator, env.layout);

  Txn first(env.base, env.undo, env.layout, appender_a, env.timestamps, 0, context_a);
  Txn second(env.base, env.undo, env.layout, appender_b, env.timestamps, 1, context_b);

  first.Begin();
  second.Begin();

  const RowId row = env.layout.RowIdAt(7);
  ASSERT_EQ(first.Update(row, ValueFor(1, env.layout.data_size())), UpdateOutcome::kApplied);
  EXPECT_EQ(second.Update(row, ValueFor(2, env.layout.data_size())), UpdateOutcome::kConflict);

  first.Commit();
  second.Abort();

  // 冲突方没有写入任何行版本，提交方那一个版本必须还在。
  const ScanResult result = env.scanner.ScanBaseline(env.timestamps.ReadTimestamp());
  std::vector<uint32_t> state(kTableRows, kInitial);
  state[7] = 1;
  EXPECT_EQ(result.checksum, ExpectedChecksum(state, env.layout.data_size()));
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
        // 单事务只有几微秒，整个循环可能落在一个调度时间片里，四个线程完全
        // 串行——那样永远撞不出冲突。让出一次，保证线程真的交错。
        std::this_thread::yield();
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

// 提交应用窗口：闸在第二行写回前合上——row_a 已带 commit_ts，row_b 还锁着。
// §8 的 min 机制必须让此刻开始的读者取到 read_ts = t1 的 commit_ts，两行都沿
// undo 链回溯到旧值，快照完整落在 T1 之前；写冲突检测在窗口内同样成立。
TEST(MvccConcurrency, CommitApplyWindowKeepsSnapshotConsistent) {
  Env env;
  TxnContext context_t1(2, env.layout.data_size());
  TxnContext context_t2(1, env.layout.data_size());
  UndoAppender appender_t1(env.allocator, env.layout);
  UndoAppender appender_t2(env.allocator, env.layout);

  Gate gate(2);
  Txn t1(env.base, env.undo, env.layout, appender_t1, env.timestamps, 0, context_t1,
         TransactionHooks(&FireGate, &gate));
  t1.Begin();
  const RowId row_a = env.layout.RowIdAt(3);
  const RowId row_b = env.layout.RowIdAt(4);
  const std::vector<std::byte> fresh = ValueFor(1, env.layout.data_size());
  ASSERT_EQ(t1.Update(row_a, fresh), UpdateOutcome::kApplied);
  ASSERT_EQ(t1.Update(row_b, fresh), UpdateOutcome::kApplied);

  std::thread committer([&] { t1.Commit(); });
  EXPECT_TRUE(gate.WaitReached()) << "钩子未触发：后续断言基于已完成的提交/回滚";

  // row_a 已写回（锁已清、version = commit_ts），但 read_ts 不大于它——必须
  // 走链读到旧值；row_b 还锁着，同样走链。
  std::vector<std::byte> visible(env.layout.data_size());
  const Timestamp snapshot_ts = env.timestamps.ReadTimestamp();
  ReadVisible(env.base, env.undo, env.layout, row_a, snapshot_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), 0u) << "已写回的 row_a 在窗口内不可见";
  ReadVisible(env.base, env.undo, env.layout, row_b, snapshot_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), 0u) << "未写回的 row_b 读到旧值";

  // 窗口内写冲突检测照常：row_a 靠 ts 比较（版本 >= read_ts），row_b 靠锁位。
  Txn t2(env.base, env.undo, env.layout, appender_t2, env.timestamps, 1, context_t2);
  t2.Begin();
  EXPECT_EQ(t2.Update(row_a, ValueFor(2, env.layout.data_size())), UpdateOutcome::kConflict);
  EXPECT_EQ(t2.Update(row_b, ValueFor(2, env.layout.data_size())), UpdateOutcome::kConflict);

  gate.Release();
  committer.join();

  // 提交完成后新快照看到两个新版本；T2 重试正常更新并提交。
  const Timestamp after_ts = env.timestamps.ReadTimestamp();
  ASSERT_GT(after_ts, snapshot_ts);
  ReadVisible(env.base, env.undo, env.layout, row_a, after_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), ChecksumBytes(fresh));
  ReadVisible(env.base, env.undo, env.layout, row_b, after_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), ChecksumBytes(fresh));

  t2.Begin();
  ASSERT_EQ(t2.Update(row_a, ValueFor(2, env.layout.data_size())), UpdateOutcome::kApplied);
  t2.Commit();
  ReadVisible(env.base, env.undo, env.layout, row_a, env.timestamps.ReadTimestamp(),
              AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible),
            ChecksumBytes(ValueFor(2, env.layout.data_size())));
}

// 回滚窗口：闸在第一行清锁前合上，写集两行整段持锁。读者沿 undo 链读到旧值；
// 回滚完成后锁位全清，后续更新不受残留锁位影响。
TEST(MvccConcurrency, RollbackWindowResolvesLockedRowsViaUndoChain) {
  Env env;
  TxnContext context_t1(2, env.layout.data_size());
  TxnContext context_t2(1, env.layout.data_size());
  UndoAppender appender_t1(env.allocator, env.layout);
  UndoAppender appender_t2(env.allocator, env.layout);

  Gate gate(1);
  Txn t1(env.base, env.undo, env.layout, appender_t1, env.timestamps, 0, context_t1,
         TransactionHooks(&FireGate, &gate));
  t1.Begin();
  const RowId row_a = env.layout.RowIdAt(5);
  const RowId row_b = env.layout.RowIdAt(6);
  const std::vector<std::byte> fresh = ValueFor(1, env.layout.data_size());
  ASSERT_EQ(t1.Update(row_a, fresh), UpdateOutcome::kApplied);
  ASSERT_EQ(t1.Update(row_b, fresh), UpdateOutcome::kApplied);

  std::thread aborter([&] { t1.Abort(); });
  EXPECT_TRUE(gate.WaitReached()) << "钩子未触发：后续断言基于已完成的提交/回滚";

  // 两行都还锁着：读者必须沿 undo 链回溯到旧值，不能被锁位挡住或读到半态。
  std::vector<std::byte> visible(env.layout.data_size());
  const Timestamp snapshot_ts = env.timestamps.ReadTimestamp();
  ReadVisible(env.base, env.undo, env.layout, row_a, snapshot_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), 0u);
  ReadVisible(env.base, env.undo, env.layout, row_b, snapshot_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), 0u);

  gate.Release();
  aborter.join();

  // 回滚不恢复数据区（Update 阶段就没写过数据），只清锁位；值保持旧值。
  const Timestamp after_ts = env.timestamps.ReadTimestamp();
  ReadVisible(env.base, env.undo, env.layout, row_a, after_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), 0u);
  ReadVisible(env.base, env.undo, env.layout, row_b, after_ts, AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), 0u);

  Txn t2(env.base, env.undo, env.layout, appender_t2, env.timestamps, 1, context_t2);
  t2.Begin();
  ASSERT_EQ(t2.Update(row_a, ValueFor(2, env.layout.data_size())), UpdateOutcome::kApplied)
      << "回滚后不得残留锁位";
  t2.Commit();
  ReadVisible(env.base, env.undo, env.layout, row_a, env.timestamps.ReadTimestamp(),
              AccessMode::kNormal, visible);
  EXPECT_EQ(ChecksumBytes(visible), ChecksumBytes(ValueFor(2, env.layout.data_size())));
}

}  // namespace
}  // namespace mvcc
