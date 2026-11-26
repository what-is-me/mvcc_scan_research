#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>

#include "common/config.h"

namespace mvcc {

// 配置层的键分布 -> Zipf 参数。均匀分布用 theta = 0：公式在 0 处自然退化
// （zeta 比值为 1、eta 为 1），不需要给生成器另外分支。
constexpr double ZipfTheta(KeyDist dist) {
  switch (dist) {
    case KeyDist::kUniform:
      return 0.0;
    case KeyDist::kZipf05:
      return 0.5;
    case KeyDist::kZipf08:
      return 0.8;
    case KeyDist::kZipf095:
      return 0.95;
  }
  return 0.0;
}

// YCSB 的近似 Zipf：P(rank) ∝ 1/(rank+1)^theta。返回值落在 [0, item_count)。
// theta 越大越集中；逆变换要求 theta < 1（theta = 1 时 alpha 无定义，
// theta > 1 时公式不再对应标准 Zipf 分布），不接受。
class ZipfGenerator {
 public:
  ZipfGenerator(uint64_t item_count, double theta, uint64_t seed)
      : item_count_(item_count), theta_(theta), rng_(seed) {
    assert(item_count_ > 1);
    assert(theta_ >= 0.0 && theta_ < 1.0);

    const double zeta_2 = 1.0 + std::pow(0.5, theta_);
    const double zeta_n = Zeta(item_count_);
    alpha_ = 1.0 / (1.0 - theta_);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(item_count_), 1.0 - theta_)) /
           (1.0 - zeta_2 / zeta_n);
    zeta_n_ = zeta_n;
  }

  uint64_t Next() {
    const double u = uniform_(rng_);
    const double uz = u * zeta_n_;
    if (uz < 1.0) return 0;
    if (uz < 1.0 + std::pow(0.5, theta_)) return 1;

    const double rank = static_cast<double>(item_count_) * std::pow(eta_ * u - eta_ + 1.0, alpha_);
    if (rank <= 0.0) return 0;
    const auto index = static_cast<uint64_t>(rank);
    return index < item_count_ ? index : item_count_ - 1;
  }

  uint64_t item_count() const { return item_count_; }
  double theta() const { return theta_; }

 private:
  double Zeta(uint64_t n) const {
    double sum = 0.0;
    for (uint64_t index = 1; index <= n; ++index) {
      sum += std::pow(1.0 / static_cast<double>(index), theta_);
    }
    return sum;
  }

  uint64_t item_count_;
  double theta_;
  double zeta_n_ = 0.0;
  double alpha_ = 0.0;
  double eta_ = 0.0;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> uniform_{0.0, 1.0};
};

}  // namespace mvcc
