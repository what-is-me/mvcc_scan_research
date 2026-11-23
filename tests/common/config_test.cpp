#include <cstdio>
#include <fstream>
#include <limits>
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

// §11 变化范围：矩阵之外的页大小一律拒绝——容量乘法先验上限再进行，
// 回绕会让"超限报错"失效。
TEST(Config, RejectsPageSizeOutsideDocumentedRange) {
  Config config;
  config.page_size_kb = 3;
  EXPECT_NE(config.Validate().find("page_size_kb"), std::string::npos);

  config.page_size_kb = 4;  // 是扇区倍数，但不在 8/16/32/64 之内
  EXPECT_NE(config.Validate().find("page_size_kb"), std::string::npos);

  config.page_size_kb = 128;  // 2 的幂，同样超出矩阵
  EXPECT_NE(config.Validate().find("page_size_kb"), std::string::npos);

  config.page_size_kb = 8;
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

// undo 无 GC，容量必须覆盖实验时长内的全部更新：300 s × 100 万行/秒 × 128 B
// 约 36 GiB，默认的 16 GiB 不够，启动就该拒绝而不是跑到一半写满磁盘。
TEST(Config, ValidateRejectsUndoCapacityBelowTheEstimate) {
  Config config;
  config.duration_s = 300;
  EXPECT_NE(config.Validate().find("undo_capacity_gb"), std::string::npos);

  config.undo_capacity_gb = 64;
  EXPECT_EQ(config.Validate(), "");
}

TEST(Config, ValidateRejectsRowLargerThanPage) {
  Config config;
  config.page_size_kb = 8;
  config.row_size_b = 16384;
  EXPECT_NE(config.Validate().find("page size"), std::string::npos);
}

// 上限校验必须走在乘法之前：duration_s 的上限若在 undo 容量估算之后才查，
// 估算式（duration_s 是乘数）会先把限制顶爆，报出来的就是 undo 容量错误——
// 必须断言报的是 duration_s 本身，否则顺序错了测试也察觉不到。
TEST(Config, ValidateRejectsDurationAndUndoCapacityBeyondRange) {
  Config config;
  config.duration_s = 86'401;
  EXPECT_NE(config.Validate().find("duration_s is beyond"), std::string::npos);
  EXPECT_EQ(config.Validate().find("undo_capacity_gb"), std::string::npos);

  Config huge;
  huge.duration_s = std::numeric_limits<size_t>::max();
  EXPECT_NE(huge.Validate().find("duration_s is beyond"), std::string::npos);
  EXPECT_EQ(huge.Validate().find("undo_capacity_gb"), std::string::npos);

  Config capacity;
  capacity.undo_capacity_gb = 1'048'577;
  EXPECT_NE(capacity.Validate().find("undo_capacity_gb is beyond"), std::string::npos);
}

TEST(Config, ValidateRejectsTinyRowAndMisalignedRow) {
  Config config;
  config.row_size_b = 8;
  EXPECT_NE(config.Validate().find("16-byte"), std::string::npos);

  // 16 是头长本身：数据区为空，ChecksumBytes 对任何内容都返回 0。
  config.row_size_b = 16;
  EXPECT_NE(config.Validate().find("8 bytes of row data"), std::string::npos);

  config.row_size_b = 24;
  EXPECT_EQ(config.Validate(), "");

  config.row_size_b = 100;
  EXPECT_NE(config.Validate().find("multiple of 8"), std::string::npos);
}

// 提交时逐行 pin 写集页。写集比池还大时，多个事务会各自攥着一部分帧等
// 下一页，谁也放不掉；这是配置错误，必须在启动期拒绝，而不是跑到一半挂住。
TEST(Config, ValidateRejectsWriteSetLargerThanBasePool) {
  Config config;
  config.page_size_kb = 64;
  config.base_pool_mb = 1;  // 16 帧
  config.rows_per_txn = 16;
  EXPECT_EQ(config.Validate(), "");

  config.rows_per_txn = 17;
  EXPECT_NE(config.Validate().find("base pool frame count"), std::string::npos);
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
  EXPECT_STREQ(ToString(ScanPolicy::kHotFirst), "hotfirst");
  EXPECT_STREQ(ToString(ScanPolicy::kDeferChain), "defer_chain");
  EXPECT_STREQ(ToString(ScanPolicy::kDeferPaged), "defer_paged");
  EXPECT_STREQ(ToString(Replacer::kLru), "lru");
  EXPECT_STREQ(ToString(Replacer::kClock), "clock");
  EXPECT_STREQ(ToString(Replacer::kMru), "mru");
  EXPECT_STREQ(ToString(Replacer::kArc), "arc");
  EXPECT_STREQ(ToString(Replacer::kLruK), "lru_k");
}

}  // namespace mvcc
