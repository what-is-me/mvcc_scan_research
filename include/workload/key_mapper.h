#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace mvcc {

enum class KeyMapping {
  kScattered,
  kHead,
};

// 逻辑热点排名 -> 物理行号。scattered 用固定种子的置换表把热点均匀打散到全表：
// 直接映射会让 Zipf 把竞争压到表头几页，行锁与写冲突全部集中在那里（§11）。
class KeyMapper {
 public:
  KeyMapper(uint64_t row_count, KeyMapping mapping, uint64_t seed) : mapping_(mapping) {
    assert(row_count > 0);
    if (mapping_ != KeyMapping::kScattered) return;

    permutation_.resize(row_count);
    std::iota(permutation_.begin(), permutation_.end(), uint64_t{0});
    std::mt19937_64 rng(seed);
    std::shuffle(permutation_.begin(), permutation_.end(), rng);
  }

  uint64_t Map(uint64_t rank) const {
    return mapping_ == KeyMapping::kHead ? rank : permutation_[rank];
  }

 private:
  KeyMapping mapping_;
  std::vector<uint64_t> permutation_;
};

}  // namespace mvcc
