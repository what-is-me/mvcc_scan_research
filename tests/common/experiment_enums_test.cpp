#include <type_traits>

#include <gtest/gtest.h>

// 同一个 TU 同时包含配置层与实现层的头。两个层各自定义过 ScanPolicy /
// KeyMapping 时，这里会直接编译失败——装配层（读 Config、驱动 TableScanner、
// 建 KeyMapper）必然是这个场景。
#include "common/config.h"
#include "scan/table_scanner.h"
#include "workload/key_mapper.h"

namespace mvcc {
namespace {

static_assert(std::is_same_v<std::remove_cvref_t<decltype(Config{}.scan_policy)>, ScanPolicy>);
static_assert(std::is_same_v<std::remove_cvref_t<decltype(Config{}.key_mapping)>, KeyMapping>);

TEST(ExperimentEnums, ScanPolicyParsedFromConfigReachesTheScanner) {
  Config config;
  std::string error;
  ASSERT_TRUE(config.Set("scan_policy", "hotfirst", &error)) << error;
  EXPECT_EQ(config.scan_policy, ScanPolicy::kHotFirst);
  EXPECT_STREQ(ToString(config.scan_policy), "hotfirst");
}

TEST(ExperimentEnums, KeyMappingParsedFromConfigReachesTheMapper) {
  Config config;
  std::string error;
  ASSERT_TRUE(config.Set("key_mapping", "head", &error)) << error;
  EXPECT_EQ(config.key_mapping, KeyMapping::kHead);

  const KeyMapper mapper(16, config.key_mapping, 7);
  EXPECT_EQ(mapper.Map(5), 5u);
}

// ScanAccessMode 与 AccessMode 的枚举项顺序相反（scan/normal 对 normal/scan），
// 按序号 static_cast 会把语义整个反转。这条用例把"顺序确实相反"和"具名转换
// 正确"同时钉住——将来谁把两个枚举改成同序，这条会提醒他去删掉这条防护。
TEST(ExperimentEnums, ScanAccessModeConvertsByNameNotByOrdinal) {
  EXPECT_EQ(ToAccessMode(ScanAccessMode::kScan), AccessMode::kScan);
  EXPECT_EQ(ToAccessMode(ScanAccessMode::kNormal), AccessMode::kNormal);
  EXPECT_NE(static_cast<int>(ScanAccessMode::kScan), static_cast<int>(AccessMode::kScan));
}

}  // namespace
}  // namespace mvcc
