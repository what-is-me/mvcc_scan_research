#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace mvcc {

// 事务延迟的分位数统计。每事务一条延迟会随实验时长无界增长（60 s x
// 数十万事务/s），改用对数分桶直方图：桶宽按 2^(1/8) 倍数增长，几十个桶
// 覆盖 1 us 到小时级，内存固定、跨线程合并就是逐桶相加。
//
// 分位数在桶内线性插值，误差小于桶宽（约 9%）；对 p50/p99 的报告口径足够，
// 且不会像水库采样那样依赖样本顺序。
class LatencyHistogram {
 public:
  // 桶下界 = 2^(exp) us，每倍程再分 2^kSubBits 份。index 从 0（< 2 us）
  // 到 kBucketCount-1（约 2^53 us），覆盖任何现实延迟。
  static constexpr int kSubBits = 3;
  static constexpr int kMaxExponent = 50;
  static constexpr int kBucketCount = (kMaxExponent + 1) << kSubBits;

  void AddMicros(uint64_t latency_us) {
    ++buckets_[BucketOf(latency_us)];
    ++count_;
  }

  void Merge(const LatencyHistogram& other) {
    for (int index = 0; index < kBucketCount; ++index) buckets_[index] += other.buckets_[index];
    count_ += other.count_;
  }

  uint64_t count() const { return count_; }

  // p ∈ (0, 1]，返回毫秒。样本数不足以给出该分位时返回 0。
  double PercentileMs(double p) const {
    assert(p > 0.0 && p <= 1.0);
    if (count_ == 0) return 0.0;

    // 第 target 小的样本（1 起计）落在哪个桶，就在桶内线性插值。
    const uint64_t target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_)));
    uint64_t seen = 0;
    for (int index = 0; index < kBucketCount; ++index) {
      if (buckets_[index] == 0) continue;
      if (seen + buckets_[index] < target) {
        seen += buckets_[index];
        continue;
      }
      const double lower = BucketLowerBoundUs(index);
      const double upper = BucketLowerBoundUs(index + 1);
      // 桶内均匀假设：第 (target - seen) / bucket_count 个位置。
      const double fraction =
          static_cast<double>(target - seen) / static_cast<double>(buckets_[index]);
      return (lower + (upper - lower) * fraction) / 1000.0;
    }
    return 0.0;
  }

 private:
  static int BucketOf(uint64_t latency_us) {
    if (latency_us < 2) return 0;
    int exponent = 0;
    for (uint64_t value = latency_us; value > 1; value >>= 1) ++exponent;
    if (exponent > kMaxExponent) exponent = kMaxExponent;
    // 分数位是最高位之后的 3 位；指数不足 kSubBits 时后面是隐含的零，左移
    // 补齐再取低位。右移负数是未定义行为——x86 把移位计数对 64 取模，
    // 2~7 µs 会塌进错误的桶，分位数直接失真。
    int fraction;
    if (exponent >= kSubBits) {
      fraction = static_cast<int>((latency_us >> (exponent - kSubBits)) & ((1 << kSubBits) - 1));
    } else {
      fraction = static_cast<int>((latency_us << (kSubBits - exponent)) & ((1 << kSubBits) - 1));
    }
    const int index = (exponent << kSubBits) | fraction;
    return index < kBucketCount ? index : kBucketCount - 1;
  }

  static double BucketLowerBoundUs(int index) {
    if (index <= 0) return 0.0;
    const int exponent = index >> kSubBits;
    const int fraction = index & ((1 << kSubBits) - 1);
    const double unit = static_cast<double>(uint64_t{1} << exponent);
    return unit * (1.0 + static_cast<double>(fraction) / static_cast<double>(1 << kSubBits));
  }

  std::array<uint64_t, kBucketCount> buckets_{};
  uint64_t count_ = 0;
};

}  // namespace mvcc
