#include <cstdint>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "tp/key_mapper.h"
#include "tp/zipf.h"

namespace mvcc {
namespace {

TEST(ZipfGenerator, StaysInsideRange) {
  constexpr uint64_t kItems = 10000;
  ZipfGenerator zipf(kItems, 0.8, 1);

  for (size_t index = 0; index < 200000; ++index) {
    ASSERT_LT(zipf.Next(), kItems);
  }
}

TEST(ZipfGenerator, IsReproducibleForSameSeed) {
  ZipfGenerator left(5000, 0.8, 7);
  ZipfGenerator right(5000, 0.8, 7);

  for (size_t index = 0; index < 1000; ++index) {
    ASSERT_EQ(left.Next(), right.Next());
  }
}

// YCSB 的 Zipf 使最小键最热。theta 越大，头部占的比例越高。
TEST(ZipfGenerator, SkewGrowsWithTheta) {
  constexpr uint64_t kItems = 10000;
  constexpr size_t kSamples = 200000;
  constexpr uint64_t kHead = 100;  // 前 1%

  const auto head_share = [](double theta) {
    ZipfGenerator zipf(kItems, theta, 11);
    size_t hits = 0;
    for (size_t index = 0; index < kSamples; ++index) {
      if (zipf.Next() < kHead) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(kSamples);
  };

  const double steep = head_share(0.8);
  const double shallow = head_share(0.5);

  // 理论值 theta=0.8 约 0.29、theta=0.5 约 0.09；留足采样误差。
  EXPECT_GT(steep, 0.20);
  EXPECT_LT(shallow, 0.15);
  EXPECT_GT(steep, shallow);
}

TEST(KeyMapper, ScatteredMapsOntoFullPermutation) {
  constexpr uint64_t kRows = 10000;
  const KeyMapper mapper(kRows, KeyMapping::kScattered, 3);

  std::set<uint64_t> seen;
  for (uint64_t rank = 0; rank < kRows; ++rank) {
    const uint64_t row = mapper.Map(rank);
    ASSERT_LT(row, kRows);
    ASSERT_TRUE(seen.insert(row).second) << "rank " << rank << " maps to a duplicate row";
  }
  EXPECT_EQ(seen.size(), kRows);
}

TEST(KeyMapper, HeadIsIdentity) {
  constexpr uint64_t kRows = 1000;
  const KeyMapper mapper(kRows, KeyMapping::kHead, 3);

  for (uint64_t rank = 0; rank < kRows; ++rank) {
    EXPECT_EQ(mapper.Map(rank), rank);
  }
}

// 配置层的四种键分布必须都能落到可用的 theta 上：均匀用 0，公式在 0 处
// 退化为等概率，不需要给生成器另开分支。
TEST(ZipfGenerator, EveryConfiguredDistributionProducesAUsableTheta) {
  EXPECT_DOUBLE_EQ(ZipfTheta(KeyDist::kUniform), 0.0);
  EXPECT_DOUBLE_EQ(ZipfTheta(KeyDist::kZipf05), 0.5);
  EXPECT_DOUBLE_EQ(ZipfTheta(KeyDist::kZipf08), 0.8);
  EXPECT_DOUBLE_EQ(ZipfTheta(KeyDist::kZipf095), 0.95);

  constexpr uint64_t kRows = 64;
  ZipfGenerator uniform(kRows, ZipfTheta(KeyDist::kUniform), 1);
  std::vector<size_t> counts(kRows, 0);
  for (size_t draw = 0; draw < 64'000; ++draw) {
    ++counts[uniform.Next()];
  }

  // 均匀分布下每个 rank 的期望是 1000 次，允许 ±30% 的采样波动。
  for (const size_t count : counts) {
    EXPECT_GT(count, 700u);
    EXPECT_LT(count, 1300u);
  }
}

}  // namespace
}  // namespace mvcc
