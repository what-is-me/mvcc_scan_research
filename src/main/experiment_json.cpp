#include "main/experiment.h"

#include <cstdio>
#include <string>

namespace mvcc {
namespace {

std::string Number(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}

// experiment_id 是唯一会进 JSON 的自由字符串；路径等一律不落盘。
std::string Escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

// 字段顺序按 §12 固定；details 数组必须是最后一个字段（跨实验的汇总脚本
// 依赖这个位置来区分配置段与明细段）。
void WriteExperimentJson(const ExperimentSummary& summary, const std::filesystem::path& path) {
  const Config& config = summary.config;
  std::string json = "{\n";
  json += "  \"experiment_id\": \"" + Escape(summary.experiment_id) + "\",\n";
  json += "  \"scan_policy\": \"" + std::string(ToString(config.scan_policy)) + "\",\n";
  json += "  \"replacer\": \"" + std::string(ToString(config.replacer)) + "\",\n";
  json += "  \"page_size_kb\": " + std::to_string(config.page_size_kb) + ",\n";
  json += "  \"row_size_b\": " + std::to_string(config.row_size_b) + ",\n";
  json += "  \"table_rows\": " + std::to_string(summary.actual_table_rows) + ",\n";
  json += "  \"base_pool_mb\": " + std::to_string(config.base_pool_mb) + ",\n";
  json += "  \"undo_pool_mb\": " + std::to_string(config.undo_pool_mb) + ",\n";
  json += "  \"tp_threads\": " + std::to_string(config.tp_threads) + ",\n";
  json += "  \"key_dist\": \"" + std::string(ToString(config.key_dist)) + "\",\n";
  json += "  \"key_mapping\": \"" + std::string(ToString(config.key_mapping)) + "\",\n";
  json += "  \"rows_per_txn\": " + std::to_string(config.rows_per_txn) + ",\n";
  json += "  \"scan_interval_ms\": " + std::to_string(config.scan_interval_ms) + ",\n";
  json += "  \"scan_access_mode\": \"" + std::string(ToString(config.scan_access_mode)) + "\",\n";
  json += "  \"duration_s\": " + std::to_string(config.duration_s) + ",\n";
  json += "  \"scan_count\": " + std::to_string(summary.scan_count) + ",\n";
  json += "  \"scan_latency_ms_p50\": " + Number(summary.scan_latency_ms_p50) + ",\n";
  json += "  \"scan_latency_ms_p99\": " + Number(summary.scan_latency_ms_p99) + ",\n";
  json += "  \"main_scan_ms_p50\": " + Number(summary.main_scan_ms_p50) + ",\n";
  json += "  \"resolve_ms_p50\": " + Number(summary.resolve_ms_p50) + ",\n";
  json += "  \"base_logical_reads\": " + std::to_string(summary.base_logical_reads) + ",\n";
  json += "  \"base_physical_reads\": " + std::to_string(summary.base_physical_reads) + ",\n";
  json += "  \"base_physical_read_ops\": " + std::to_string(summary.base_physical_read_ops) + ",\n";
  json += "  \"base_hit_rate\": " + Number(summary.base_hit_rate) + ",\n";
  json += "  \"undo_logical_reads\": " + std::to_string(summary.undo_logical_reads) + ",\n";
  json += "  \"undo_physical_reads\": " + std::to_string(summary.undo_physical_reads) + ",\n";
  json += "  \"undo_physical_read_ops\": " + std::to_string(summary.undo_physical_read_ops) + ",\n";
  json += "  \"undo_hit_rate\": " + Number(summary.undo_hit_rate) + ",\n";
  json += "  \"tp_throughput_txn_per_s\": " + Number(summary.tp_throughput_txn_per_s) + ",\n";
  json += "  \"tp_latency_ms_p50\": " + Number(summary.tp_latency_ms_p50) + ",\n";
  json += "  \"tp_latency_ms_p99\": " + Number(summary.tp_latency_ms_p99) + ",\n";
  json += "  \"write_conflict_rate\": " + Number(summary.write_conflict_rate) + ",\n";
  json += "  \"undo_file_mb_end\": " + Number(summary.undo_file_mb_end) + ",\n";
  json += "  \"chain_avg_hops\": " + Number(summary.chain_avg_hops) + ",\n";

  json += "  \"details\": [\n";
  for (size_t index = 0; index < summary.details.size(); ++index) {
    const ScanDetail& detail = summary.details[index];
    json += "    {\"scan_seq\": " + std::to_string(detail.scan_seq);
    json += ", \"start_time_s\": " + Number(detail.start_time_s);
    json += ", \"main_scan_ms\": " + Number(detail.main_scan_ms);
    json += ", \"resolve_ms\": " + Number(detail.resolve_ms);
    json += ", \"chain_rows\": " + std::to_string(detail.chain_rows);
    json += ", \"chain_hops\": " + std::to_string(detail.chain_hops);
    json += ", \"base_logical_reads\": " + std::to_string(detail.base_logical_reads);
    json += ", \"base_physical_reads\": " + std::to_string(detail.base_physical_reads);
    json += ", \"base_physical_read_ops\": " + std::to_string(detail.base_physical_read_ops);
    json += ", \"undo_logical_reads\": " + std::to_string(detail.undo_logical_reads);
    json += ", \"undo_physical_reads\": " + std::to_string(detail.undo_physical_reads);
    json += ", \"undo_physical_read_ops\": " + std::to_string(detail.undo_physical_read_ops);
    json += index + 1 == summary.details.size() ? "}\n" : "},\n";
  }
  json += "  ]\n}\n";

  FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    throw std::runtime_error("cannot open result file: " + path.string());
  }
  const size_t written = std::fwrite(json.data(), 1, json.size(), file);
  const int closed = std::fclose(file);
  if (written != json.size() || closed != 0) {
    throw std::runtime_error("cannot write result file: " + path.string());
  }
}

}  // namespace mvcc
