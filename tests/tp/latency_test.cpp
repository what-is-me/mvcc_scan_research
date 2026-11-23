#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "tp/latency.h"

namespace mvcc {
namespace {

TEST(LatencyHistogram, PercentileFallsInsideTheDominantBucket) {
  LatencyHistogram histogram;
  for (int index = 0; index < 100; ++index) histogram.AddMicros(1000);
  // 单值分布全部落在一个桶里，插值给出桶边界；对 1 ms 的量化误差 < 9%。
  const double p50 = histogram.PercentileMs(0.50);
  EXPECT_GT(p50, 0.9);
  EXPECT_LT(p50, 1.1);
  EXPECT_EQ(histogram.count(), 100u);
}

TEST(LatencyHistogram, PercentilesAreMonotonic) {
  LatencyHistogram histogram;
  for (int index = 0; index < 900; ++index) histogram.AddMicros(500);
  for (int index = 0; index < 90; ++index) histogram.AddMicros(5000);
  for (int index = 0; index < 9; ++index) histogram.AddMicros(50000);
  // 500us 桶 [500, 568)，5ms 桶 [4.608, 5.12)ms，50ms 桶 [46.08, 51.2)ms。
  EXPECT_DOUBLE_EQ(histogram.count(), 999);
  EXPECT_GT(histogram.PercentileMs(0.50), 0.4);
  EXPECT_LT(histogram.PercentileMs(0.50), 0.7);
  EXPECT_GT(histogram.PercentileMs(0.99), 4.0);
  EXPECT_LT(histogram.PercentileMs(0.99), 6.0);
  EXPECT_GE(histogram.PercentileMs(0.99), histogram.PercentileMs(0.50));
}

TEST(LatencyHistogram, EmptyHistogramReportsZero) {
  LatencyHistogram histogram;
  EXPECT_EQ(histogram.PercentileMs(0.5), 0.0);
  EXPECT_EQ(histogram.count(), 0u);
}

TEST(LatencyHistogram, MergeAddsCountsAndBuckets) {
  LatencyHistogram left;
  left.AddMicros(1000);
  left.AddMicros(1000);
  LatencyHistogram right;
  right.AddMicros(8000);

  left.Merge(right);
  EXPECT_EQ(left.count(), 3u);
  // 两个 1 ms、一个 8 ms：中位数落在 1 ms 桶，最大值落在 8 ms 桶。
  EXPECT_GT(left.PercentileMs(0.5), 0.9);
  EXPECT_LT(left.PercentileMs(0.5), 1.1);
  EXPECT_GT(left.PercentileMs(1.0), 7.0);
}

TEST(LatencyHistogram, HandlesSubMicrosecondAndHugeValues) {
  LatencyHistogram histogram;
  histogram.AddMicros(0);
  histogram.AddMicros(1);
  histogram.AddMicros(10'000'000);  // 10 s
  EXPECT_GE(histogram.PercentileMs(0.5), 0.0);
  EXPECT_GT(histogram.PercentileMs(0.99), 1000.0);
}

// 2~7 µs 的指数不足 kSubBits，分数位要左移补齐：右移负数在 x86 上被掩码，
// 这些值会全部塌进倍程的第一个桶，分位数失真超过一个桶宽。
TEST(LatencyHistogram, SmallValuesLandInTheirOwnSubBuckets) {
  LatencyHistogram three_us;
  for (int index = 0; index < 10; ++index) three_us.AddMicros(3);
  // 3 µs 落在 [3.0, 3.25) µs；塌进 [2, 2.25) 会低于 3 µs 下界。
  EXPECT_GE(three_us.PercentileMs(0.5), 0.003);
  EXPECT_LT(three_us.PercentileMs(0.5), 0.0033);

  LatencyHistogram six_us;
  for (int index = 0; index < 10; ++index) six_us.AddMicros(6);
  EXPECT_GE(six_us.PercentileMs(0.5), 0.006);
  EXPECT_LT(six_us.PercentileMs(0.5), 0.007);
}

}  // namespace
}  // namespace mvcc
