#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/arc_replacer.h"
#include "buffer/buffer_pool.h"
#include "buffer/clock_replacer.h"
#include "buffer/lru_k_replacer.h"
#include "buffer/lru_replacer.h"
#include "buffer/mru_replacer.h"
#include "buffer/page_table.h"
#include "memory_page_file.h"
#include "storage/page.h"
#include "storage/table_layout.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kRowsPerPage = kPageSize / kRowSize;
constexpr size_t kPages = 24;
constexpr size_t kFrames = 8;
constexpr size_t kThreads = 6;
constexpr size_t kOpsPerThread = 20000;
constexpr size_t kPrefetchPages = 4;

// ARC 的构造参数多一个页号上界，其余策略只吃帧数。
template <typename Replacer>
struct ReplacerFactory;

template <>
struct ReplacerFactory<LruReplacer> {
  static LruReplacer Make(size_t frames, size_t) { return LruReplacer(frames); }
};

template <>
struct ReplacerFactory<ClockReplacer> {
  static ClockReplacer Make(size_t frames, size_t) { return ClockReplacer(frames); }
};

template <>
struct ReplacerFactory<MruReplacer> {
  static MruReplacer Make(size_t frames, size_t) { return MruReplacer(frames); }
};

template <>
struct ReplacerFactory<LruKReplacer> {
  static LruKReplacer Make(size_t frames, size_t) { return LruKReplacer(frames); }
};

template <>
struct ReplacerFactory<ArcReplacer> {
  static ArcReplacer Make(size_t frames, size_t pages) { return ArcReplacer(frames, pages); }
};

class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed) {}

  size_t Below(size_t bound) {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return static_cast<size_t>(state_ % bound);
  }

 private:
  uint64_t state_;
};

uint64_t FirstWord(const Frame* frame) {
  uint64_t value = 0;
  std::memcpy(&value, frame->data.data(), sizeof(value));
  return value;
}

template <typename Replacer>
class PoolStress : public ::testing::Test {
 protected:
  // 随机并发混合 Fix / Unpin / MarkDirty / Prefetch，周期性机械校验不变量。
  // 五种策略跑同一套：ARC 的 tier / 幽灵双向链表、Clock 的 hand 与引用位都只在
  // 并发下才会被踩坏，单线程的选帧用例看不出来。
  void RunStress() {
    const TableLayout layout(kPageSize, kRowSize, kPages * kRowsPerPage);
    test::MemoryPageFile file(layout.page_count(), layout.page_size());
    using Pool = BufferPool<DirectPageTable, Replacer>;
    Pool pool(layout, kFrames, file, RowLockMode::kAllocate,
              DirectPageTable(layout.page_count()),
              ReplacerFactory<Replacer>::Make(kFrames, layout.page_count()), PoolHooks(),
              kPrefetchPages);

    std::atomic<size_t> failures{0};
    std::atomic<size_t> wrong_content{0};
    std::vector<std::thread> workers;

    for (size_t thread = 0; thread < kThreads; ++thread) {
      workers.emplace_back([&pool, &failures, &wrong_content, thread] {
        Rng rng(0x9E3779B97F4A7C15ull * (thread + 1));
        for (size_t op = 0; op < kOpsPerThread; ++op) {
          const size_t choice = rng.Below(100);
          if (choice < 8) {
            pool.Prefetch(rng.Below(kPages), kPrefetchPages, AccessMode::kScan);
          } else {
            const PageId page = rng.Below(kPages);
            Frame* frame = pool.Fix(page, AccessMode::kNormal);
            if (FirstWord(frame) != page) wrong_content.fetch_add(1);
            if (choice < 20) pool.MarkDirty(frame);
            pool.Unpin(frame);
          }

          if (op % 64 == 0) {
            std::string error;
            if (!pool.CheckInvariants(&error)) {
              ADD_FAILURE() << error;
              failures.fetch_add(1);
              return;
            }
          }
        }
      });
    }
    for (auto& worker : workers) worker.join();

    EXPECT_EQ(failures.load(), 0u);
    EXPECT_EQ(wrong_content.load(), 0u);

    std::string error;
    EXPECT_TRUE(pool.CheckInvariants(&error)) << error;
    EXPECT_GT(pool.stats().physical_reads, 0u);
  }
};

using ReplacerTypes =
    ::testing::Types<LruReplacer, ClockReplacer, MruReplacer, LruKReplacer, ArcReplacer>;

TYPED_TEST_SUITE(PoolStress, ReplacerTypes);

TYPED_TEST(PoolStress, RandomConcurrentAccessKeepsInvariants) { this->RunStress(); }

}  // namespace
}  // namespace mvcc
