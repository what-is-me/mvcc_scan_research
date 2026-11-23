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

void PrintUsage() {
  std::cerr << "用法: mvcc_exp [config文件] [key=value ...] [--experiment-id=ID]\n"
            << "  config文件默认为 config/default.cfg；key=value 覆盖其中一项。\n"
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
  std::vector<std::pair<std::string, std::string>> overrides;

  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg.rfind("--experiment-id=", 0) == 0) {
      experiment_id = arg.substr(sizeof("--experiment-id=") - 1);
      explicit_id = true;
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
  for (const std::string& warning : config.Warnings()) {
    std::cerr << "警告: " << warning << "\n";
  }
  if (!explicit_id) experiment_id = DefaultExperimentId();

  std::cout << "实验配置: " << config.Describe() << "\n";
  std::cout << "experiment_id = " << experiment_id << "\n";

  try {
    const ExperimentSummary summary = RunExperiment(config, experiment_id);
    const std::filesystem::path output =
        std::filesystem::path(config.results_dir) / ("exp_" + experiment_id + ".json");
    std::filesystem::create_directories(output.parent_path());
    WriteExperimentJson(summary, output);

    std::cout << "完成: 扫描 " << summary.scan_count << " 次，TP 吞吐 "
              << static_cast<long long>(summary.tp_throughput_txn_per_s) << " txn/s，冲突率 "
              << summary.write_conflict_rate << "\n";
    std::cout << "结果文件: " << output.string() << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "实验失败: " << ex.what() << "\n";
    return 2;
  }
  return 0;
}
