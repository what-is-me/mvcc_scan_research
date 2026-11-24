#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "buffer/buffer_pool.h"
#include "common/config.h"

namespace mvcc {

// 一次扫描的明细，对应结果 JSON 里 details[] 的一个元素。五个池计数是本次
// 扫描**自己**的增量（池在调用线程内累加到 ScanResult 的 sink 上），不含
// 并发 TP 的流量——池级计数器是所有线程共享的，取前后差会把别人的读写算
// 到扫描头上。
struct ScanDetail {
  uint64_t scan_seq = 0;  // 自 1 起计
  double start_time_s = 0.0;
  double main_scan_ms = 0.0;
  double resolve_ms = 0.0;
  uint64_t chain_rows = 0;
  uint64_t chain_hops = 0;
  uint64_t base_logical_reads = 0;
  uint64_t base_physical_reads = 0;
  uint64_t base_physical_read_ops = 0;
  uint64_t undo_logical_reads = 0;
  uint64_t undo_physical_reads = 0;
  uint64_t undo_physical_read_ops = 0;
};

// 聚合结果 = 配置快照 + 顶层指标 + details（写 JSON 时按 §12 的字段顺序输出）。
// 顶层池指标是各次扫描增量的累加，与 details 逐列求和一致。
struct ExperimentSummary {
  std::string experiment_id;
  Config config;
  // 实际行数：行数向上取整到页边界（§6），oracle 与扫描都以它为准。
  uint64_t actual_table_rows = 0;

  size_t scan_count = 0;
  double scan_latency_ms_p50 = 0.0;
  double scan_latency_ms_p99 = 0.0;
  double main_scan_ms_p50 = 0.0;
  double resolve_ms_p50 = 0.0;
  uint64_t base_logical_reads = 0;
  uint64_t base_physical_reads = 0;
  uint64_t base_physical_read_ops = 0;
  double base_hit_rate = 0.0;
  uint64_t undo_logical_reads = 0;
  uint64_t undo_physical_reads = 0;
  uint64_t undo_physical_read_ops = 0;
  double undo_hit_rate = 0.0;
  double tp_throughput_txn_per_s = 0.0;
  double tp_latency_ms_p50 = 0.0;
  double tp_latency_ms_p99 = 0.0;
  double write_conflict_rate = 0.0;
  double undo_file_mb_end = 0.0;
  double chain_avg_hops = 0.0;

  std::vector<ScanDetail> details;
};

// 校验配置并跑一轮实验（TP 负载 + AP 扫描按间隔触发），结束后删除本轮的
// 表文件，返回聚合结果。配置非法时抛 std::invalid_argument。
ExperimentSummary RunExperiment(const Config& config, const std::string& experiment_id);

// 串行跑完整个矩阵（扫描策略 5 × 替换策略 4，按枚举声明顺序），每轮
// experiment_id = 前缀_策略名_替换器名，并立即写出 exp_<id>.json——已完成
// 的轮次不因后续轮次失败而丢失。某轮抛出即终止矩阵，异常继续传播。
// 其余维度沿用 base_config；矩阵不聚合，横向汇总由脚本从 JSON 生成（§12）。
std::vector<ExperimentSummary> RunMatrix(const Config& base_config,
                                         const std::string& experiment_prefix);

// 手写序列化（§12：不引入第三方 JSON 库）。结构固定：顶层字段按规范顺序，
// details 数组最后。path 的父目录必须已存在。
void WriteExperimentJson(const ExperimentSummary& summary, const std::filesystem::path& path);

// 取 p 分位数（p ∈ (0, 1]）。空序列返回 0；否则返回第 ⌈p·n⌉ 小的值。
double Percentile(std::vector<double> values, double p);

}  // namespace mvcc
