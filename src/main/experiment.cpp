#include "main/experiment.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "buffer/arc_replacer.h"
#include "buffer/clock_replacer.h"
#include "buffer/lru_k_replacer.h"
#include "buffer/lru_replacer.h"
#include "buffer/mru_replacer.h"
#include "main/env.h"
#include "storage/table_builder.h"
#include "tp/key_mapper.h"
#include "tp/tp_runner.h"

namespace mvcc {
namespace {

constexpr uint64_t kMaxTxnsPerThread = 1'000'000'000;

double SecondsSince(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// ARC 还需要页号上界（幽灵表按页号稠密索引），其余策略只要帧数。
template <typename BaseReplacer>
BaseReplacer MakeBaseReplacer(size_t frames, size_t page_count) {
  if constexpr (std::is_same_v<BaseReplacer, ArcReplacer>) {
    return BaseReplacer(frames, page_count);
  } else {
    return BaseReplacer(frames);
  }
}

ScanDetail MakeDetail(uint64_t seq, double start_s, const ScanResult& result) {
  ScanDetail detail;
  detail.scan_seq = seq;
  detail.start_time_s = start_s;
  detail.main_scan_ms = result.main_scan_ms;
  detail.resolve_ms = result.resolve_ms;
  detail.chain_rows = result.chain_rows;
  detail.chain_hops = result.chain_hops;
  detail.base_logical_reads = result.base_stats.logical_reads;
  detail.base_physical_reads = result.base_stats.physical_reads;
  detail.base_physical_read_ops = result.base_stats.physical_read_ops;
  detail.undo_logical_reads = result.undo_stats.logical_reads;
  detail.undo_physical_reads = result.undo_stats.physical_reads;
  detail.undo_physical_read_ops = result.undo_stats.physical_read_ops;
  return detail;
}

ScanDetail operator+(const ScanDetail& left, const ScanDetail& right) {
  ScanDetail sum;
  sum.chain_rows = left.chain_rows + right.chain_rows;
  sum.chain_hops = left.chain_hops + right.chain_hops;
  sum.base_logical_reads = left.base_logical_reads + right.base_logical_reads;
  sum.base_physical_reads = left.base_physical_reads + right.base_physical_reads;
  sum.base_physical_read_ops = left.base_physical_read_ops + right.base_physical_read_ops;
  sum.undo_logical_reads = left.undo_logical_reads + right.undo_logical_reads;
  sum.undo_physical_reads = left.undo_physical_reads + right.undo_physical_reads;
  sum.undo_physical_read_ops = left.undo_physical_read_ops + right.undo_physical_read_ops;
  return sum;
}

double HitRate(uint64_t logical_reads, uint64_t logical_misses) {
  if (logical_reads == 0) return 0.0;
  return 1.0 - static_cast<double>(logical_misses) / static_cast<double>(logical_reads);
}

// 每次实验重建文件：BuildBaseFile 自己删旧文件；undo 只追加，残留会让新一轮
// 从旧页号上继续写，同样必须删。
template <typename BaseReplacer>
ExperimentSummary RunExperimentWith(const Config& config, const std::string& experiment_id) {
  namespace fs = std::filesystem;
  const fs::path dir(config.results_dir);
  fs::create_directories(dir);
  const fs::path base_path = dir / (experiment_id + "_base.tbl");
  const fs::path undo_path = dir / (experiment_id + "_undo.tbl");

  const TableLayout layout(config.page_size_kb * 1024, config.row_size_b, config.table_rows);
  BuildBaseFile(base_path, layout);
  RemoveFileIfExists(undo_path);

  // env 关在一个作用域里：析构关闭文件句柄后，表文件才能删除（Windows 不允许
  // 删除打开着的文件）。线程都在块内 join，块外只剩纯数据。
  double undo_file_mb_end = 0.0;
  double elapsed_s = 0.0;
  uint64_t base_misses = 0;
  uint64_t undo_misses = 0;
  TpOutcome outcome;
  std::vector<ScanDetail> details;
  {
    BaseReplacer replacer =
        MakeBaseReplacer<BaseReplacer>(ExperimentEnv<BaseReplacer>::BaseFrames(config),
                                       layout.page_count());
    ExperimentEnv<BaseReplacer> env(config, base_path, undo_path, std::move(replacer));

    TpConfig tp;
    tp.threads = config.tp_threads;
    tp.rows_per_txn = config.rows_per_txn;
    tp.txns_per_thread = kMaxTxnsPerThread;  // 实际由 stop 旗标按时长结束
    tp.key_dist = config.key_dist;
    tp.mapping = config.key_mapping;
    tp.record_log = false;  // benchmark 不留提交日志；正确性 oracle 属测试层
    const KeyMapper mapper(layout.row_count(), tp.mapping, tp.seed);

    const auto started = std::chrono::steady_clock::now();
    std::atomic<bool> stop{false};
    std::vector<TpOutcome> partial(config.tp_threads);
    std::vector<std::thread> workers;
    workers.reserve(config.tp_threads);
    for (size_t thread = 0; thread < config.tp_threads; ++thread) {
      workers.emplace_back([&, thread] {
        RunTpWorker(env.base_pool, env.undo_pool, env.layout, env.allocator, env.timestamps,
                    mapper, tp, thread, &stop, partial[thread]);
      });
    }

    // AP 扫描线程：先扫一次，再按间隔触发。间隔分片等待，停止请求不必等完
    // 一个完整间隔；停止后的那一次扫描不再发起，在途的扫完即退出。
    std::thread ap([&] {
      uint64_t seq = 0;
      for (;;) {
        const double start_s = SecondsSince(started);
        const Timestamp read_ts = env.timestamps.ReadTimestamp();
        const ScanResult result = env.scanner.Scan(read_ts, config.scan_policy);
        details.push_back(MakeDetail(++seq, start_s, result));
        base_misses += result.base_stats.logical_misses;
        undo_misses += result.undo_stats.logical_misses;

        if (stop.load(std::memory_order_acquire)) break;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(config.scan_interval_ms);
        while (!stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    });

    const auto deadline = started + std::chrono::seconds(config.duration_s);
    std::this_thread::sleep_until(deadline);
    stop.store(true, std::memory_order_release);
    for (std::thread& worker : workers) worker.join();
    ap.join();
    elapsed_s = SecondsSince(started);

    outcome = MergeTpOutcomes(partial);
    undo_file_mb_end = static_cast<double>(env.undo_file.size_in_pages()) *
                       static_cast<double>(layout.page_size()) / (1024.0 * 1024.0);
  }  // env 析构，句柄关闭

  ExperimentSummary summary;
  summary.experiment_id = experiment_id;
  summary.config = config;
  summary.actual_table_rows = layout.row_count();

  ScanDetail total;
  std::vector<double> scan_latencies;
  std::vector<double> main_scan_ms;
  std::vector<double> resolve_ms;
  scan_latencies.reserve(details.size());
  for (const ScanDetail& detail : details) {
    total = total + detail;
    scan_latencies.push_back(detail.main_scan_ms + detail.resolve_ms);
    main_scan_ms.push_back(detail.main_scan_ms);
    resolve_ms.push_back(detail.resolve_ms);
  }
  summary.scan_count = details.size();
  summary.scan_latency_ms_p50 = Percentile(scan_latencies, 0.50);
  summary.scan_latency_ms_p99 = Percentile(scan_latencies, 0.99);
  summary.main_scan_ms_p50 = Percentile(main_scan_ms, 0.50);
  summary.resolve_ms_p50 = Percentile(resolve_ms, 0.50);

  summary.base_logical_reads = total.base_logical_reads;
  summary.base_physical_reads = total.base_physical_reads;
  summary.base_physical_read_ops = total.base_physical_read_ops;
  summary.base_hit_rate = HitRate(summary.base_logical_reads, base_misses);
  summary.undo_logical_reads = total.undo_logical_reads;
  summary.undo_physical_reads = total.undo_physical_reads;
  summary.undo_physical_read_ops = total.undo_physical_read_ops;
  summary.undo_hit_rate = HitRate(summary.undo_logical_reads, undo_misses);

  summary.tp_throughput_txn_per_s =
      elapsed_s > 0 ? static_cast<double>(outcome.committed) / elapsed_s : 0.0;
  summary.tp_latency_ms_p50 = outcome.latency.PercentileMs(0.50);
  summary.tp_latency_ms_p99 = outcome.latency.PercentileMs(0.99);
  const uint64_t attempts = outcome.committed + outcome.conflicts + outcome.write_set_full;
  summary.write_conflict_rate =
      attempts > 0 ? static_cast<double>(outcome.conflicts) / static_cast<double>(attempts) : 0.0;

  summary.undo_file_mb_end = undo_file_mb_end;
  summary.chain_avg_hops =
      total.chain_rows > 0
          ? static_cast<double>(total.chain_hops) / static_cast<double>(total.chain_rows)
          : 0.0;
  summary.details = std::move(details);

  // 表文件不保留：每轮实验重建（BuildBaseFile 也会删旧文件），JSON 是唯一
  // 交付物。删除失败不视为实验失败——下一轮同 id 会再删一次。
  std::error_code cleanup_error;
  fs::remove(base_path, cleanup_error);
  fs::remove(undo_path, cleanup_error);
  return summary;
}

}  // namespace

ExperimentSummary RunExperiment(const Config& config, const std::string& experiment_id) {
  if (const std::string error = config.Validate(); !error.empty()) {
    throw std::invalid_argument("invalid config: " + error);
  }

  switch (config.replacer) {
    case Replacer::kLru:
      return RunExperimentWith<LruReplacer>(config, experiment_id);
    case Replacer::kClock:
      return RunExperimentWith<ClockReplacer>(config, experiment_id);
    case Replacer::kMru:
      return RunExperimentWith<MruReplacer>(config, experiment_id);
    case Replacer::kArc:
      return RunExperimentWith<ArcReplacer>(config, experiment_id);
    case Replacer::kLruK:
      return RunExperimentWith<LruKReplacer>(config, experiment_id);
  }
  throw std::invalid_argument("unknown replacer");
}

double Percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  assert(p > 0.0 && p <= 1.0);
  std::sort(values.begin(), values.end());
  const size_t rank =
      std::min(static_cast<size_t>(std::ceil(p * static_cast<double>(values.size()))),
               values.size());
  return values[rank - 1];
}

}  // namespace mvcc
