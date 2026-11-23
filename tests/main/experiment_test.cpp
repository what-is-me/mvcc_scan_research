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

Config TinyConfig(const fs::path& dir) {
  Config config;
  config.table_rows = 512;  // 64 KB 页 / 128 B 行 = 恰好 1 页
  config.base_pool_mb = 1;
  config.undo_pool_mb = 1;
  config.undo_capacity_gb = 1;
  config.tp_threads = 2;
  config.key_dist = KeyDist::kUniform;
  config.scan_interval_ms = 200;
  config.duration_s = 1;
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
TEST(Experiment, TinyRunProducesConsistentSummaryAndJson) {
  const fs::path dir = fs::temp_directory_path() / "mvcc_exp_smoke";
  fs::remove_all(dir);
  fs::create_directories(dir);

  const Config config = TinyConfig(dir);
  const ExperimentSummary summary = RunExperiment(config, "smoke");

  EXPECT_GT(summary.actual_table_rows, 0u);
  EXPECT_EQ(summary.actual_table_rows, 512u);
  EXPECT_GT(summary.scan_count, 1u);
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

// §11：间隔是相邻两次扫描起始时刻之差，不是"结束后再等"。断言用本次扫描
// 自己的耗时自校准：新语义下起始间隔 = max(扫描耗时, 间隔)，恒小于
// "扫描耗时 + 间隔"；旧实现（从扫描结束起算）恰好等于后者，5 ms 余量把它
// 与睡眠切片的抖动区分开。
TEST(Experiment, ScanIntervalIsMeasuredBetweenScanStarts) {
  Config config;
  config.table_rows = 800000;  // 1563 页：一次扫描明显长于毫秒级，间隔可分辨
  config.base_pool_mb = 16;
  config.undo_pool_mb = 8;
  config.undo_capacity_gb = 1;
  config.tp_threads = 2;
  config.key_dist = KeyDist::kUniform;
  config.scan_interval_ms = 100;
  config.duration_s = 1;
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

}  // namespace
}  // namespace mvcc
