#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "main/experiment.h"

namespace mvcc {
namespace {

namespace fs = std::filesystem;

// TSan 插桩对内存访问密集的路径有 15~20 倍减速：端到端计时窗口按倍数放大，
// 让断言既不隐含机器速度下限、也不隐含"非插桩速度"（§12）。matrix 用例的
// 断言只要求跑出 ≥1 轮，duration 保持下限，避免 20 轮把套件时长拖爆。
#ifdef MVCC_SANITIZER_THREAD
constexpr int kTsanTimeScale = 8;
#else
constexpr int kTsanTimeScale = 1;
#endif

// 冒烟配置的每个维度只服务一件事：让"undo 淘汰写回"在极少量的更新尝试内
// 必然发生，CI runner 的停顿与 TSan 插桩都挡不住它。理由见 TinyRun 的说明。
Config TinyConfig(const fs::path& dir) {
  Config config;
  config.table_rows = 512;   // 64 KB 页 / 4 KB 行 = 16 行/页 → 32 页
  config.row_size_b = 4096;  // undo 记录与基表行同宽（§4）：每 undo 页 16 条
  config.base_pool_mb = 1;   // 16 帧：写集 ≤ 8 页 + 扫描 1 帧，提交期 pin 不饥饿
  config.undo_pool_mb = 1;   // 16 帧 × 16 条 = 256 槽
  // 满速估算（TSan 窗口 8 s × 1M 行/s × 4 KB）也要过防打穿校验。PagedFile
  // 按需增长，这个数字不占磁盘。
  config.undo_capacity_gb = 33;
  config.tp_threads = 2;
  config.rows_per_txn = 8;  // 每次尝试占 8 槽 → 32 次尝试必触发首次淘汰
  config.key_dist = KeyDist::kUniform;
  config.scan_interval_ms = 200;
  config.duration_s = 1 * kTsanTimeScale;
  config.results_dir = dir.string();
  return config;
}

std::string ReadAll(const fs::path& path) {
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

size_t PositionOf(const std::string& text, const std::string& key) {
  const size_t position = text.find("\"" + key + "\":");
  EXPECT_NE(position, std::string::npos) << key << " missing";
  return position;
}

TEST(Percentile, PicksTheRankInASortedCopy) {
  std::vector<double> values;
  for (int index = 1; index <= 100; ++index) values.push_back(static_cast<double>(index));

  EXPECT_DOUBLE_EQ(Percentile(values, 0.50), 50.0);
  EXPECT_DOUBLE_EQ(Percentile(values, 0.99), 99.0);
  EXPECT_DOUBLE_EQ(Percentile(values, 1.0), 100.0);
  EXPECT_DOUBLE_EQ(Percentile({}, 0.5), 0.0);
  EXPECT_DOUBLE_EQ(Percentile({7.0}, 0.99), 7.0);
  // 101 个样本时 p50 = 51：rank 必须向上取整，截断会拿 50。
  values.push_back(101.0);
  EXPECT_DOUBLE_EQ(Percentile(values, 0.50), 51.0);
  // 不重排输入副本之外的调用方数据。
  EXPECT_EQ(values.front(), 1.0);
}

TEST(ExperimentJson, EmitsTopLevelFieldsInSpecOrderAndDetailsLast) {
  ExperimentSummary summary;
  summary.experiment_id = "e1";
  summary.config = TinyConfig(".");
  summary.actual_table_rows = 512;
  summary.scan_count = 2;
  summary.scan_latency_ms_p50 = 1.5;
  summary.scan_latency_ms_p99 = 2.5;
  summary.base_logical_reads = 100;
  summary.base_physical_reads = 20;
  summary.base_physical_read_ops = 3;
  summary.base_hit_rate = 0.8;
  summary.undo_logical_reads = 10;
  summary.undo_physical_reads = 4;
  summary.undo_physical_read_ops = 2;
  summary.undo_hit_rate = 0.6;
  summary.tp_throughput_txn_per_s = 1234.5;
  summary.tp_latency_ms_p50 = 0.1;
  summary.tp_latency_ms_p99 = 0.4;
  summary.write_conflict_rate = 0.05;
  summary.undo_file_mb_end = 0.125;
  summary.chain_avg_hops = 1.25;

  ScanDetail detail;
  detail.scan_seq = 1;
  detail.start_time_s = 0.001;
  detail.main_scan_ms = 1.0;
  detail.resolve_ms = 0.5;
  detail.chain_rows = 7;
  detail.chain_hops = 9;
  detail.base_logical_reads = 100;
  detail.undo_physical_reads = 4;
  summary.details.push_back(detail);

  const fs::path path = fs::temp_directory_path() / "mvcc_exp_json_test" / "out.json";
  fs::create_directories(path.parent_path());
  WriteExperimentJson(summary, path);
  const std::string text = ReadAll(path);
  fs::remove_all(path.parent_path());

  // 顶层字段按规范顺序出现。
  const std::vector<std::string> order = {
      "experiment_id", "scan_policy", "replacer", "page_size_kb", "row_size_b", "table_rows",
      "base_pool_mb", "undo_pool_mb", "tp_threads", "key_dist", "key_mapping", "rows_per_txn",
      "scan_interval_ms", "scan_access_mode", "duration_s", "scan_count", "scan_latency_ms_p50",
      "scan_latency_ms_p99", "main_scan_ms_p50", "resolve_ms_p50", "base_logical_reads",
      "base_physical_reads", "base_physical_read_ops", "base_hit_rate", "undo_logical_reads",
      "undo_physical_reads", "undo_physical_read_ops", "undo_hit_rate", "tp_throughput_txn_per_s",
      "tp_latency_ms_p50", "tp_latency_ms_p99", "write_conflict_rate", "undo_file_mb_end",
      "chain_avg_hops"};
  size_t previous = 0;
  for (const std::string& key : order) {
    const size_t position = PositionOf(text, key);
    EXPECT_GT(position, previous) << key << " out of order";
    previous = position;
  }
  // details 数组在所有顶层字段之后、文档末尾。
  const size_t details = text.find("\"details\": [");
  EXPECT_NE(details, std::string::npos);
  EXPECT_GT(details, previous);
  EXPECT_GT(text.find("\"scan_seq\": 1"), details);
  EXPECT_NE(text.find("\"scan_seq\": 1, \"start_time_s\": 0.001, \"main_scan_ms\": 1.000"),
            std::string::npos);
  EXPECT_NE(text.find("]\n}\n"), std::string::npos);

  // 明细行内 12 个字段同样按规范顺序出现。搜索起点锚在 details 之后，避免
  // 匹配到顶层的同名字段。
  const std::vector<std::string> detail_order = {
      "scan_seq", "start_time_s", "main_scan_ms", "resolve_ms",
      "chain_rows", "chain_hops", "base_logical_reads", "base_physical_reads",
      "base_physical_read_ops", "undo_logical_reads", "undo_physical_reads",
      "undo_physical_read_ops"};
  size_t detail_previous = details;
  for (const std::string& key : detail_order) {
    const size_t position = text.find("\"" + key + "\":", detail_previous);
    EXPECT_NE(position, std::string::npos) << key << " missing in detail row";
    EXPECT_GT(position, detail_previous) << key << " out of order in detail row";
    detail_previous = position;
  }
}

TEST(ExperimentJson, EscapesTheExperimentId) {
  ExperimentSummary summary;
  summary.experiment_id = "a\"b\\c";
  summary.config = TinyConfig(".");

  const fs::path path = fs::temp_directory_path() / "mvcc_exp_json_test" / "escaped.json";
  fs::create_directories(path.parent_path());
  WriteExperimentJson(summary, path);
  const std::string text = ReadAll(path);
  fs::remove_all(path.parent_path());

  EXPECT_NE(text.find("\"experiment_id\": \"a\\\"b\\\\c\""), std::string::npos);
}

// 端到端冒烟：真实文件 + direct I/O，配置取最小可跑规模。跑通即验证装配
// 顺序、线程编排与 JSON 落盘；benchmark 不做正确性校验（§12），断言只盯
// 计数与文件形状。
//
// 断言不得隐含机器速度下限——CI runner 的调度停顿可达数百毫秒，本用例在
// CI 上挂过两次，两次都是这条：
// - scan_count 只断言 ≥ 1。AP 线程能否在 1 s 内被调度多次是调度运气，不是
//   系统性质；相邻扫描起始间隔的语义由 ScanIntervalIsMeasuredBetweenScanStarts
//   专项断言（那边间隔取扫描耗时的数倍，余量以数量级计）。
// - undo_file_mb_end > 0 要求跑出至少一次淘汰写回。undo 记录与基表行同宽
//   （§4），undo 池容量 = 池字节 / 行宽；行宽 128 B 时容量 8192 槽，CI 停顿
//   窗口内根本填不满。行宽提到 4 KB（每 undo 页 16 条、池 256 槽）、
//   rows_per_txn=8，把门槛压到 32 次更新尝试——冲突事务同样消费 undo 槽位
//   （Reserve 先于冲突检查，§9.3），按 attempts 计，TSan 插桩 + 停顿也拦不住；
//   TSan 构建再把计时窗口放大 8 倍（kTsanTimeScale）。
TEST(Experiment, TinyRunProducesConsistentSummaryAndJson) {
  const fs::path dir = fs::temp_directory_path() / "mvcc_exp_smoke";
  fs::remove_all(dir);
  fs::create_directories(dir);

  const Config config = TinyConfig(dir);
  const ExperimentSummary summary = RunExperiment(config, "smoke");

  EXPECT_GT(summary.actual_table_rows, 0u);
  EXPECT_EQ(summary.actual_table_rows, 512u);
  EXPECT_GE(summary.scan_count, 1u);
  EXPECT_GT(summary.tp_throughput_txn_per_s, 0.0);
  EXPECT_GT(summary.tp_latency_ms_p50, 0.0);
  EXPECT_GT(summary.undo_file_mb_end, 0.0);
  ASSERT_EQ(summary.details.size(), summary.scan_count);
  for (size_t index = 0; index < summary.details.size(); ++index) {
    EXPECT_EQ(summary.details[index].scan_seq, index + 1);
    if (index > 0) {
      EXPECT_GE(summary.details[index].start_time_s, summary.details[index - 1].start_time_s);
    }
    EXPECT_GE(summary.details[index].main_scan_ms, 0.0);
  }

  // §12：顶层池指标是各次扫描增量的累加，与 details 逐列求和一致。
  uint64_t base_logical = 0, base_physical = 0, base_ops = 0;
  uint64_t undo_logical = 0, undo_physical = 0, undo_ops = 0;
  for (const ScanDetail& detail : summary.details) {
    base_logical += detail.base_logical_reads;
    base_physical += detail.base_physical_reads;
    base_ops += detail.base_physical_read_ops;
    undo_logical += detail.undo_logical_reads;
    undo_physical += detail.undo_physical_reads;
    undo_ops += detail.undo_physical_read_ops;
  }
  EXPECT_EQ(summary.base_logical_reads, base_logical);
  EXPECT_EQ(summary.base_physical_reads, base_physical);
  EXPECT_EQ(summary.base_physical_read_ops, base_ops);
  EXPECT_EQ(summary.undo_logical_reads, undo_logical);
  EXPECT_EQ(summary.undo_physical_reads, undo_physical);
  EXPECT_EQ(summary.undo_physical_read_ops, undo_ops);
  // 表文件已删除；JSON 由入口层写，这里自己写一份来检查产物形状。
  EXPECT_FALSE(fs::exists(dir / "smoke_base.tbl"));
  EXPECT_FALSE(fs::exists(dir / "smoke_undo.tbl"));

  const fs::path json_path = dir / "exp_smoke.json";
  WriteExperimentJson(summary, json_path);
  const std::string text = ReadAll(json_path);
  EXPECT_NE(text.find("\"scan_count\": " + std::to_string(summary.scan_count)), std::string::npos);
  EXPECT_NE(text.find("  ]\n}\n"), std::string::npos);
  fs::remove_all(dir);
}

// §11：间隔是相邻两次扫描起始时刻之差，不是"结束后再等"。
//
// 区分度完全来自上一轮的扫描耗时 prev：起始起算的起始间隔 = max(prev, 间隔)，
// 结束起算 = prev + 间隔。两者只在 prev < 间隔的轮次可分辨，差距 = prev。
// 因此间隔必须取扫描耗时的数倍，让每一轮都落在可分辨区域，且差距
// （≈ 扫描耗时，百毫秒级）远大于两轮之间那条调度路径的偶发毛刺——扫描
// 线程跑满一个时间片后被换出，恢复延迟可达几十毫秒。间隔贴着扫描耗时的
// 时候（扫描 > 间隔）两种实现行为相同、断言余量全部暴露给调度毛刺，
// 这正是这条用例曾经偶发失败的原因。
// 末尾的 5 ms 不再是给唤醒过冲的余量，而是保证"结束后再等"的实现
// （delta ≥ prev + 间隔，只会更大）必定落网。
TEST(Experiment, ScanIntervalIsMeasuredBetweenScanStarts) {
  Config config;
  config.table_rows = 800000;  // 1563 页：本机实测扫描约 100~260 ms
  config.base_pool_mb = 16;
  config.undo_pool_mb = 8;
  config.undo_capacity_gb = 16;  // TSan 下 16 s × 满速 1M 行/s × 128 B = 2 GB，给足即可
  config.tp_threads = 2;
  config.key_dist = KeyDist::kUniform;
  config.scan_interval_ms = 600;  // 数倍于扫描耗时：每一轮都落在可分辨区域
  config.duration_s = 2 * kTsanTimeScale;
  config.results_dir = (std::filesystem::temp_directory_path() / "mvcc_exp_interval").string();

  std::filesystem::remove_all(config.results_dir);
  const ExperimentSummary summary = RunExperiment(config, "interval");

  ASSERT_GE(summary.details.size(), 2u);
  for (size_t index = 1; index < summary.details.size(); ++index) {
    const double delta =
        summary.details[index].start_time_s - summary.details[index - 1].start_time_s;
    const double previous_ms = summary.details[index - 1].main_scan_ms +
                               summary.details[index - 1].resolve_ms;
    EXPECT_GT(delta, 0.0);
    EXPECT_LT(delta, previous_ms / 1000.0 + config.scan_interval_ms / 1000.0 - 0.005)
        << "scan " << index << ": delta=" << delta << " previous_ms=" << previous_ms;
  }
  std::filesystem::remove_all(config.results_dir);
}

TEST(Experiment, RejectsInvalidConfigBeforeBuildingAnything) {
  Config config = TinyConfig(".");
  config.rows_per_txn = 1'000'000;  // 远超基表池帧数（16），启动即拒
  EXPECT_THROW((void)RunExperiment(config, "bad"), std::invalid_argument);
}

// §13 步骤 10：矩阵运行。20 轮按枚举声明顺序逐个产出 JSON，id 编码策略组合，
// 表文件每轮清理。每轮约 1 s（duration_s 下限），是套件里最贵的一条。
TEST(Experiment, MatrixRunsEveryScanPolicyReplacerPair) {
  const fs::path dir = fs::temp_directory_path() / "mvcc_exp_matrix";
  fs::remove_all(dir);
  fs::create_directories(dir);

  Config config = TinyConfig(dir);
  config.scan_interval_ms = 100;
  config.duration_s = 1;  // 断言只要 ≥1 轮：20 轮 × duration 是套件时长，保持下限
  const std::vector<ExperimentSummary> summaries = RunMatrix(config, "matrix");

  ASSERT_EQ(summaries.size(), 5u * 4u);
  const char* kScanPolicies[] = {"baseline", "hotfirst", "hotfirst_bypass", "defer_chain",
                                 "defer_paged"};
  const char* kReplacers[] = {"lru", "clock", "arc", "lru_k"};
  size_t index = 0;
  for (const char* policy : kScanPolicies) {
    for (const char* replacer : kReplacers) {
      const ExperimentSummary& summary = summaries[index];
      const std::string expected_id = std::string("matrix_") + policy + "_" + replacer;
      EXPECT_EQ(summary.experiment_id, expected_id) << "轮次 " << index;
      EXPECT_GT(summary.scan_count, 0u) << expected_id;
      EXPECT_FALSE(fs::exists(dir / (expected_id + "_base.tbl"))) << expected_id;
      EXPECT_FALSE(fs::exists(dir / (expected_id + "_undo.tbl"))) << expected_id;
      // 每轮 JSON 已落盘，id 与策略字段都指向同一轮。
      const std::string text = ReadAll(dir / ("exp_" + expected_id + ".json"));
      EXPECT_NE(text.find("\"experiment_id\": \"" + expected_id + "\""), std::string::npos)
          << expected_id;
      EXPECT_NE(text.find(std::string("\"scan_policy\": \"") + policy + "\""), std::string::npos)
          << expected_id;
      EXPECT_NE(text.find(std::string("\"replacer\": \"") + replacer + "\""), std::string::npos)
          << expected_id;
      ++index;
    }
  }
  fs::remove_all(dir);
}

TEST(Experiment, RejectsInvalidMatrixConfigBeforeRunningAnything) {
  Config config = TinyConfig(".");
  config.rows_per_txn = 1'000'000;
  EXPECT_THROW((void)RunMatrix(config, "bad"), std::invalid_argument);
}

}  // namespace
}  // namespace mvcc
