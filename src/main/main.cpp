#include <chrono>
#include <cstddef>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "common/build_info.h"
#include "common/config.h"
#include "main/experiment.h"

namespace {

// experiment_id 默认取启动时刻：同目录下不会撞名，跨实验的文件也按时间
// 排序。命令行可显式指定以便重跑同一编号。
std::string DefaultExperimentId() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local);
  return buffer;
}

// 解析逗号分隔的正整数列表（池档位），如 "32,64,128"。空串视作未提供，
// 返回 false；任何一段为空、非数字或为零都拒绝——档位错误要启动即报，
// 不能留一半解析结果继续跑。
bool ParseTiers(const std::string& text, std::vector<size_t>* out) {
  if (text.empty()) return false;
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const std::string token =
        text.substr(start, comma == std::string::npos ? comma : comma - start);
    size_t value = 0;
    try {
      value = static_cast<size_t>(std::stoull(token));
    } catch (...) {
      return false;
    }
    if (token.empty() || value == 0) return false;
    out->push_back(value);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return true;
}

// 档位列表的展示形式：未提供时回退到基准配置的单值。"实验配置"行打印的是
// 档位应用之前的基准 config，档位必须单独回显——否则 CI 日志里看不出
// 列表被吃进去了，容易误判成参数丢失。
std::string FormatTiers(const std::vector<size_t>& tiers, size_t fallback) {
  if (tiers.empty()) return std::to_string(fallback);
  std::string joined;
  for (const size_t value : tiers) {
    if (!joined.empty()) joined += ',';
    joined += std::to_string(value);
  }
  return joined;
}

void PrintUsage() {
  std::cerr << "用法: mvcc_exp [--matrix] [config文件] [key=value ...] [--experiment-id=ID]\n"
            << "  config文件默认为 config/default.cfg；key=value 覆盖其中一项。\n"
            << "  --matrix 按 5×4 矩阵逐轮跑完，--experiment-id 作为各轮 id 的前缀。\n"
            << "  --base-pool-mb=32,64,128  基表池档位列表（逗号分隔），矩阵按档位\n"
            << "                            笛卡尔积扩展，id 前缀追加 _bp<值>_up<值>。\n"
            << "  --undo-pool-mb=16,32,64   undo 池档位列表，语义同上。\n"
            << "  结果写入 results_dir 下的 exp_<id>.json。\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace mvcc;
  std::cout << "MVCC 全表扫描实验原型 | " << BuildSummary() << "\n";

  Config config;
  std::string config_path = "config/default.cfg";
  std::string experiment_id;
  bool explicit_id = false;
  bool matrix = false;
  std::vector<size_t> base_pool_tiers;
  std::vector<size_t> undo_pool_tiers;
  std::vector<std::pair<std::string, std::string>> overrides;

  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg.rfind("--experiment-id=", 0) == 0) {
      experiment_id = arg.substr(sizeof("--experiment-id=") - 1);
      explicit_id = true;
      continue;
    }
    if (arg == "--matrix") {
      matrix = true;
      continue;
    }
    if (arg.rfind("--base-pool-mb=", 0) == 0) {
      if (!ParseTiers(arg.substr(sizeof("--base-pool-mb=") - 1), &base_pool_tiers)) {
        std::cerr << "非法的池档位列表: " << arg << "\n";
        PrintUsage();
        return 1;
      }
      continue;
    }
    if (arg.rfind("--undo-pool-mb=", 0) == 0) {
      if (!ParseTiers(arg.substr(sizeof("--undo-pool-mb=") - 1), &undo_pool_tiers)) {
        std::cerr << "非法的池档位列表: " << arg << "\n";
        PrintUsage();
        return 1;
      }
      continue;
    }
    if (arg.rfind("--", 0) == 0) {
      std::cerr << "未知选项: " << arg << "\n";
      PrintUsage();
      return 1;
    }
    const size_t equals = arg.find('=');
    if (equals == std::string::npos) {
      config_path = arg;  // 不含 '=' 的位置参数是配置文件路径
    } else {
      overrides.emplace_back(arg.substr(0, equals), arg.substr(equals + 1));
    }
  }

  std::string error;
  if (!LoadConfigFile(config_path, &config, &error)) {
    std::cerr << error << "\n";
    PrintUsage();
    return 1;
  }
  for (const auto& [key, value] : overrides) {
    if (!config.Set(key, value, &error)) {
      std::cerr << error << "\n";
      PrintUsage();
      return 1;
    }
  }
  if (const std::string problem = config.Validate(); !problem.empty()) {
    std::cerr << "配置非法: " << problem << "\n";
    return 1;
  }
  if (!matrix && (base_pool_tiers.size() > 1 || undo_pool_tiers.size() > 1)) {
    std::cerr << "池档位列表仅在 --matrix 模式下支持\n";
    PrintUsage();
    return 1;
  }
  if (!matrix) {
    if (!base_pool_tiers.empty()) config.base_pool_mb = base_pool_tiers.front();
    if (!undo_pool_tiers.empty()) config.undo_pool_mb = undo_pool_tiers.front();
    if (const std::string problem = config.Validate(); !problem.empty()) {
      std::cerr << "配置非法: " << problem << "\n";
      return 1;
    }
  }
  for (const std::string& warning : config.Warnings()) {
    std::cerr << "警告: " << warning << "\n";
  }
  if (!explicit_id) experiment_id = DefaultExperimentId();

  std::cout << "实验配置: " << config.Describe() << "\n";
  if (!base_pool_tiers.empty() || !undo_pool_tiers.empty()) {
    const size_t combos = (base_pool_tiers.empty() ? 1 : base_pool_tiers.size()) *
                          (undo_pool_tiers.empty() ? 1 : undo_pool_tiers.size());
    std::cout << "池档位: base_pool_mb = " << FormatTiers(base_pool_tiers, config.base_pool_mb)
              << " | undo_pool_mb = " << FormatTiers(undo_pool_tiers, config.undo_pool_mb)
              << "（" << combos << " 组档位组合，每组一个 5×4 矩阵）\n";
  }
  std::cout << "experiment_id = " << experiment_id << "\n";

  try {
    if (matrix) {
      // 矩阵模式：RunMatrix 逐轮写出 exp_<id>.json，失败即终止（已完成各轮
      // 已落盘）。experiment_id 在此处作为前缀使用；池档位列表对全部组合
      // 按笛卡尔积各跑一遍，多档时前缀追加 _bp<值>_up<值>。
      const std::vector<size_t> base_tiers = base_pool_tiers.empty()
                                                 ? std::vector<size_t>{config.base_pool_mb}
                                                 : base_pool_tiers;
      const std::vector<size_t> undo_tiers = undo_pool_tiers.empty()
                                                 ? std::vector<size_t>{config.undo_pool_mb}
                                                 : undo_pool_tiers;
      // 全组合先逐个校验再开跑：任一档非法即整批拒绝，不落任何盘。
      for (const size_t base : base_tiers) {
        for (const size_t undo : undo_tiers) {
          Config tier = config;
          tier.base_pool_mb = base;
          tier.undo_pool_mb = undo;
          if (const std::string problem = tier.Validate(); !problem.empty()) {
            std::cerr << "配置非法 (base_pool_mb=" << base << ", undo_pool_mb="
                      << undo << "): " << problem << "\n";
            return 1;
          }
        }
      }
      size_t total = 0;
      for (const size_t base : base_tiers) {
        for (const size_t undo : undo_tiers) {
          Config tier = config;
          tier.base_pool_mb = base;
          tier.undo_pool_mb = undo;
          std::string prefix = experiment_id;
          if (base_tiers.size() > 1 || undo_tiers.size() > 1) {
            prefix += "_bp" + std::to_string(base) + "_up" + std::to_string(undo);
          }
          const std::vector<ExperimentSummary> summaries = RunMatrix(tier, prefix);
          total += summaries.size();
          std::cout << "矩阵完成: bp" << base << "/up" << undo << "，共 "
                    << summaries.size() << " 轮\n";
        }
      }
      std::cout << "全部完成: " << total << " 轮，结果目录 " << config.results_dir << "\n";
    } else {
      const ExperimentSummary summary = RunExperiment(config, experiment_id);
      const std::filesystem::path output =
          std::filesystem::path(config.results_dir) / ("exp_" + experiment_id + ".json");
      std::filesystem::create_directories(output.parent_path());
      WriteExperimentJson(summary, output);

      std::cout << "完成: 扫描 " << summary.scan_count << " 次，TP 吞吐 "
                << static_cast<long long>(summary.tp_throughput_txn_per_s) << " txn/s，冲突率 "
                << summary.write_conflict_rate << "\n";
      std::cout << "结果文件: " << output.string() << "\n";
    }
  } catch (const std::exception& ex) {
    std::cerr << "实验失败: " << ex.what() << "\n";
    return 2;
  }
  return 0;
}
