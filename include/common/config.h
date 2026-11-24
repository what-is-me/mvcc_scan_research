#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mvcc {

// 实验维度的枚举只有这一份定义。扫描器、负载层、替换策略都从这里取，
// 各自另立一份会在装配层（同一 TU 同时包含配置与实现头）直接编译失败。
enum class KeyDist { kUniform, kZipf05, kZipf08, kZipf11 };
enum class KeyMapping { kScattered, kHead };
enum class ScanAccessMode { kScan, kNormal };
enum class ScanPolicy {
  kBaseline,
  kHotFirst,
  kHotFirstBypass,
  kBypassDeferPaged,
  kDeferChain,
  kDeferPaged
};
enum class Replacer { kLru, kClock, kArc, kLruK };

const char* ToString(KeyDist value);
const char* ToString(KeyMapping value);
const char* ToString(ScanAccessMode value);
const char* ToString(ScanPolicy value);
const char* ToString(Replacer value);

struct Config {
  size_t page_size_kb = 64;
  size_t row_size_b = 128;
  size_t table_rows = 2'000'000;

  size_t base_pool_mb = 128;
  size_t undo_pool_mb = 64;
  size_t undo_capacity_gb = 16;

  size_t tp_threads = 8;
  KeyDist key_dist = KeyDist::kZipf08;
  KeyMapping key_mapping = KeyMapping::kScattered;
  size_t rows_per_txn = 1;

  ScanPolicy scan_policy = ScanPolicy::kBaseline;
  size_t scan_interval_ms = 1000;
  ScanAccessMode scan_access_mode = ScanAccessMode::kScan;

  Replacer replacer = Replacer::kLru;
  size_t duration_s = 60;
  std::string results_dir = "./results";

  bool Set(std::string_view key, std::string_view value, std::string* error);

  std::string Validate() const;
  std::vector<std::string> Warnings() const;

  std::string Describe() const;
};

// key = value 文件，逐行覆盖当前值；# 起注释。任一键非法即失败并给出位置。
bool LoadConfigFile(const std::string& path, Config* config, std::string* error);

}  // namespace mvcc
