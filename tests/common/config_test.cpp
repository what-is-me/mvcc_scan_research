#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "common/config.h"

namespace mvcc {
namespace {

constexpr const char* kTempPath = "config_test_tmp.cfg";

class TempConfigFile {
 public:
  explicit TempConfigFile(const std::string& content) {
    std::ofstream out(kTempPath);
    out << content;
  }
  ~TempConfigFile() { std::remove(kTempPath); }

  TempConfigFile(const TempConfigFile&) = delete;
  TempConfigFile& operator=(const TempConfigFile&) = delete;

  const char* path() const { return kTempPath; }
};

}  // namespace

TEST(Config, DefaultsMatchDocumentedValues) {
  const Config config;
  EXPECT_EQ(config.page_size_kb, 64u);
  EXPECT_EQ(config.row_size_b, 128u);
  EXPECT_EQ(config.table_rows, 2'000'000u);
  EXPECT_EQ(config.base_pool_mb, 128u);
  EXPECT_EQ(config.undo_pool_mb, 64u);
  EXPECT_EQ(config.undo_capacity_gb, 16u);
  EXPECT_EQ(config.tp_threads, 8u);
  EXPECT_EQ(config.key_dist, KeyDist::kZipf08);
  EXPECT_EQ(config.key_mapping, KeyMapping::kScattered);
  EXPECT_EQ(config.rows_per_txn, 1u);
  EXPECT_EQ(config.scan_policy, ScanPolicy::kBaseline);
  EXPECT_EQ(config.scan_interval_ms, 1000u);
  EXPECT_EQ(config.scan_access_mode, ScanAccessMode::kScan);
  EXPECT_EQ(config.replacer, Replacer::kLru);
  EXPECT_EQ(config.duration_s, 60u);
  EXPECT_EQ(config.results_dir, "./results");
}

TEST(Config, RejectsPageSizeNotMultipleOfSector) {
  Config config;
  config.page_size_kb = 3;
  EXPECT_NE(config.Validate().find("page_size_kb"), std::string::npos);

  config.page_size_kb = 4;
  EXPECT_EQ(config.Validate(), std::string{});
}

TEST(Config, SetOverridesEveryKey) {
  Config config;
  std::string error;
  ASSERT_TRUE(config.Set("page_size_kb", "16", &error)) << error;
  ASSERT_TRUE(config.Set("row_size_b", "256", &error)) << error;
  ASSERT_TRUE(config.Set("table_rows", "8000000", &error)) << error;
  ASSERT_TRUE(config.Set("base_pool_mb", "32", &error)) << error;
  ASSERT_TRUE(config.Set("undo_pool_mb", "16", &error)) << error;
  ASSERT_TRUE(config.Set("undo_capacity_gb", "4", &error)) << error;
  ASSERT_TRUE(config.Set("tp_threads", "16", &error)) << error;
  ASSERT_TRUE(config.Set("key_dist", "zipf1.1", &error)) << error;
  ASSERT_TRUE(config.Set("key_mapping", "head", &error)) << error;
  ASSERT_TRUE(config.Set("rows_per_txn", "4", &error)) << error;
  ASSERT_TRUE(config.Set("scan_policy", "defer_paged", &error)) << error;
  ASSERT_TRUE(config.Set("scan_interval_ms", "1000", &error)) << error;
  ASSERT_TRUE(config.Set("scan_access_mode", "normal", &error)) << error;
  ASSERT_TRUE(config.Set("replacer", "lru_k", &error)) << error;
  ASSERT_TRUE(config.Set("duration_s", "300", &error)) << error;
  ASSERT_TRUE(config.Set("results_dir", "../out", &error)) << error;

  EXPECT_EQ(config.page_size_kb, 16u);
  EXPECT_EQ(config.row_size_b, 256u);
  EXPECT_EQ(config.table_rows, 8'000'000u);
  EXPECT_EQ(config.base_pool_mb, 32u);
  EXPECT_EQ(config.undo_pool_mb, 16u);
  EXPECT_EQ(config.undo_capacity_gb, 4u);
  EXPECT_EQ(config.tp_threads, 16u);
  EXPECT_EQ(config.key_dist, KeyDist::kZipf11);
  EXPECT_EQ(config.key_mapping, KeyMapping::kHead);
  EXPECT_EQ(config.rows_per_txn, 4u);
  EXPECT_EQ(config.scan_policy, ScanPolicy::kDeferPaged);
  EXPECT_EQ(config.scan_interval_ms, 1000u);
  EXPECT_EQ(config.scan_access_mode, ScanAccessMode::kNormal);
  EXPECT_EQ(config.replacer, Replacer::kLruK);
  EXPECT_EQ(config.duration_s, 300u);
  EXPECT_EQ(config.results_dir, "../out");
}

TEST(Config, RejectsUnknownKey) {
  Config config;
  std::string error;
  EXPECT_FALSE(config.Set("no_such_key", "1", &error));
  EXPECT_NE(error.find("unknown key"), std::string::npos);
}

TEST(Config, RejectsMalformedNumbers) {
  Config config;
  std::string error;
  EXPECT_FALSE(config.Set("tp_threads", "abc", &error));
  EXPECT_FALSE(config.Set("tp_threads", "8x", &error));
  EXPECT_FALSE(config.Set("tp_threads", "", &error));
  EXPECT_FALSE(config.Set("tp_threads", "-1", &error));
  EXPECT_EQ(config.tp_threads, 8u);
}

TEST(Config, RejectsUnknownEnumValue) {
  Config config;
  std::string error;
  EXPECT_FALSE(config.Set("replacer", "lru2", &error));
  EXPECT_FALSE(config.Set("scan_policy", "defer", &error));
  EXPECT_EQ(config.replacer, Replacer::kLru);
  EXPECT_EQ(config.scan_policy, ScanPolicy::kBaseline);
}

TEST(Config, LoadFileAppliesOverrides) {
  const TempConfigFile file(
      "# 实验配置\n"
      "\n"
      "  page_size_kb = 4  \n"
      "tp_threads=4          # 行尾注释\n"
      "key_dist = uniform\n");

  Config config;
  std::string error;
  ASSERT_TRUE(LoadConfigFile(file.path(), &config, &error)) << error;
  EXPECT_EQ(config.page_size_kb, 4u);
  EXPECT_EQ(config.tp_threads, 4u);
  EXPECT_EQ(config.key_dist, KeyDist::kUniform);
  EXPECT_EQ(config.row_size_b, 128u);
}

TEST(Config, LoadFileReportsLineOfBadEntry) {
  const TempConfigFile file(
      "tp_threads = 4\n"
      "page_size_kb 8\n");

  Config config;
  std::string error;
  EXPECT_FALSE(LoadConfigFile(file.path(), &config, &error));
  EXPECT_NE(error.find(":2:"), std::string::npos);
}

TEST(Config, LoadFileFailsWhenMissing) {
  Config config;
  std::string error;
  EXPECT_FALSE(LoadConfigFile("no_such_config.cfg", &config, &error));
  EXPECT_NE(error.find("cannot open"), std::string::npos);
}

TEST(Config, DefaultFileMatchesBuiltInDefaults) {
  Config config;
  std::string error;
  ASSERT_TRUE(LoadConfigFile(std::string(MVCC_SOURCE_DIR) + "/config/default.cfg", &config, &error))
      << error;
  EXPECT_EQ(config.Describe(), Config().Describe());
}

TEST(Config, ValidateAcceptsDefaults) { EXPECT_EQ(Config().Validate(), ""); }

TEST(Config, ValidateRejectsRowLargerThanPage) {
  Config config;
  config.page_size_kb = 4;
  config.row_size_b = 8192;
  EXPECT_NE(config.Validate().find("page size"), std::string::npos);
}

TEST(Config, ValidateRejectsTinyRowAndMisalignedRow) {
  Config config;
  config.row_size_b = 8;
  EXPECT_NE(config.Validate().find("16-byte"), std::string::npos);

  config.row_size_b = 100;
  EXPECT_NE(config.Validate().find("multiple of 8"), std::string::npos);
}

TEST(Config, ValidateRejectsZeroThreadsAndInterval) {
  Config config;
  config.tp_threads = 0;
  EXPECT_NE(config.Validate().find("tp_threads"), std::string::npos);

  Config other;
  other.scan_interval_ms = 0;
  EXPECT_NE(other.Validate().find("scan_interval_ms"), std::string::npos);
}

TEST(Config, ValidateRejectsEmptyTable) {
  Config config;
  config.table_rows = 0;
  EXPECT_NE(config.Validate().find("table_rows"), std::string::npos);
}

TEST(Config, WarnsWhenPoolCoversWholeTable) {
  Config config;
  EXPECT_TRUE(config.Warnings().empty());

  config.table_rows = 1000;
  EXPECT_FALSE(config.Warnings().empty());
}

TEST(Config, DescribeListsEveryParameter) {
  const std::string text = Config().Describe();
  EXPECT_NE(text.find("page_size_kb=64"), std::string::npos);
  EXPECT_NE(text.find("key_dist=zipf0.8"), std::string::npos);
  EXPECT_NE(text.find("scan_policy=baseline"), std::string::npos);
  EXPECT_NE(text.find("replacer=lru"), std::string::npos);
  EXPECT_NE(text.find("results_dir=./results"), std::string::npos);
}

TEST(Config, EnumNamesMatchExperimentOutput) {
  EXPECT_STREQ(ToString(KeyDist::kUniform), "uniform");
  EXPECT_STREQ(ToString(KeyDist::kZipf05), "zipf0.5");
  EXPECT_STREQ(ToString(KeyDist::kZipf08), "zipf0.8");
  EXPECT_STREQ(ToString(KeyDist::kZipf11), "zipf1.1");
  EXPECT_STREQ(ToString(KeyMapping::kScattered), "scattered");
  EXPECT_STREQ(ToString(KeyMapping::kHead), "head");
  EXPECT_STREQ(ToString(ScanAccessMode::kScan), "scan");
  EXPECT_STREQ(ToString(ScanAccessMode::kNormal), "normal");
  EXPECT_STREQ(ToString(ScanPolicy::kBaseline), "baseline");
  EXPECT_STREQ(ToString(ScanPolicy::kHotfirst), "hotfirst");
  EXPECT_STREQ(ToString(ScanPolicy::kDeferChain), "defer_chain");
  EXPECT_STREQ(ToString(ScanPolicy::kDeferPaged), "defer_paged");
  EXPECT_STREQ(ToString(Replacer::kLru), "lru");
  EXPECT_STREQ(ToString(Replacer::kClock), "clock");
  EXPECT_STREQ(ToString(Replacer::kMru), "mru");
  EXPECT_STREQ(ToString(Replacer::kArc), "arc");
  EXPECT_STREQ(ToString(Replacer::kLruK), "lru_k");
}

}  // namespace mvcc
