#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include "common/basic_types.h"
#include "storage/table_layout.h"
#include "txn/timestamp_manager.h"
#include "txn/transaction.h"
#include "txn/txn_context.h"
#include "txn/undo_log.h"
#include "workload/key_mapper.h"
#include "workload/zipf.h"

namespace mvcc {

struct TpConfig {
  size_t threads = 8;
  size_t rows_per_txn = 1;
  uint64_t txns_per_thread = 1000;
  double zipf_theta = 0.8;
  KeyMapping mapping = KeyMapping::kScattered;
  uint64_t seed = 0x5EED5EEDull;
};

struct CommitEntry {
  Timestamp commit_ts = 0;
  RowId row;
  uint64_t serial = 0;
};

struct TpOutcome {
  std::vector<CommitEntry> commits;
  uint64_t committed = 0;
  uint64_t conflicts = 0;
  uint64_t write_set_full = 0;
};

// 值 = 重复的 (serial << 32 | 0xC0FFEE)。oracle 只凭 serial 就能独立重算，
// 不依赖运行期的任何状态。
inline void FillValue(uint64_t serial, std::span<std::byte> out) {
  const uint64_t word = (serial << 32) | 0xC0FFEEull;
  for (size_t offset = 0; offset + sizeof(word) <= out.size(); offset += sizeof(word)) {
    std::memcpy(out.data() + offset, &word, sizeof(word));
  }
}

// 一个线程的事务循环。每线程一个上下文与 undo 追加器；写冲突按 §9.6 的
// 默认负载行为放弃整个事务并计数，不重试（重试策略属于负载层，不属于内核）。
//
// 单独暴露是为了让"中途扫描"测试能自己起 TP 线程与扫描线程并发，不必经过
// RunTp 的编排；负载状态本身是线程本地的，共享的只有池与时间戳。
template <typename BasePool, typename UndoPool>
void RunTpWorker(BasePool& base, UndoPool& undo, const TableLayout& layout,
                 UndoPageAllocator& allocator, TimestampManager& timestamps,
                 const KeyMapper& mapper, const TpConfig& config, size_t thread,
                 TpOutcome& out) {
  using Txn = Transaction<BasePool, UndoPool>;

  ZipfGenerator zipf(layout.row_count(), config.zipf_theta,
                     config.seed ^ (0x9E3779B97F4A7C15ull * (thread + 1)));
  TxnContext context(config.rows_per_txn, layout.data_size());
  UndoAppender appender(allocator, layout);
  std::vector<std::byte> value(layout.data_size());
  std::vector<CommitEntry> pending;
  pending.reserve(config.rows_per_txn);

  uint64_t serial = thread * (config.txns_per_thread * config.rows_per_txn) + 1;

  for (uint64_t index = 0; index < config.txns_per_thread; ++index) {
    Txn txn(base, undo, layout, appender, timestamps, thread, context);
    txn.Begin();
    pending.clear();

    bool applied = true;
    for (size_t row_index = 0; row_index < config.rows_per_txn; ++row_index) {
      const RowId id = layout.RowIdAt(mapper.Map(zipf.Next()));
      FillValue(serial, value);
      const UpdateOutcome outcome = txn.Update(id, value);
      if (outcome != UpdateOutcome::kApplied) {
        applied = false;
        if (outcome == UpdateOutcome::kConflict) {
          ++out.conflicts;
        } else {
          ++out.write_set_full;
        }
        break;
      }
      pending.push_back(CommitEntry{0, id, serial});
      ++serial;
    }

    if (!applied) continue;

    txn.Commit();
    for (CommitEntry& entry : pending) entry.commit_ts = txn.commit_ts();
    out.commits.insert(out.commits.end(), pending.begin(), pending.end());
    ++out.committed;
  }
}

// 按 commit_ts 合并各线程日志。同一事务的条目 ts 相同，stable_sort 保持
// 写入顺序，回放时后者覆盖前者。
inline TpOutcome MergeTpOutcomes(std::vector<TpOutcome>& partial) {
  TpOutcome outcome;
  for (TpOutcome& part : partial) {
    outcome.commits.insert(outcome.commits.end(), part.commits.begin(), part.commits.end());
    outcome.committed += part.committed;
    outcome.conflicts += part.conflicts;
    outcome.write_set_full += part.write_set_full;
  }
  std::stable_sort(outcome.commits.begin(), outcome.commits.end(),
                   [](const CommitEntry& left, const CommitEntry& right) {
                     return left.commit_ts < right.commit_ts;
                   });
  return outcome;
}

template <typename BasePool, typename UndoPool>
TpOutcome RunTp(BasePool& base, UndoPool& undo, const TableLayout& layout,
                UndoPageAllocator& allocator, TimestampManager& timestamps,
                const TpConfig& config) {
  assert(config.threads > 0);
  assert(config.threads <= timestamps.slot_count());
  assert(config.rows_per_txn > 0);

  const KeyMapper mapper(layout.row_count(), config.mapping, config.seed);
  std::vector<TpOutcome> partial(config.threads);

  std::vector<std::thread> workers;
  workers.reserve(config.threads);
  for (size_t thread = 0; thread < config.threads; ++thread) {
    workers.emplace_back([&, thread] {
      RunTpWorker(base, undo, layout, allocator, timestamps, mapper, config, thread,
                  partial[thread]);
    });
  }
  for (std::thread& worker : workers) worker.join();

  return MergeTpOutcomes(partial);
}

}  // namespace mvcc
