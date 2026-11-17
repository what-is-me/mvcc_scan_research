#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
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

using Pool = BufferPool<DirectPageTable, LruReplacer>;

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

}  // namespace

// 随机并发混合 Fix / Unpin / MarkDirty / Prefetch，周期性机械校验不变量。
TEST(PoolStress, RandomConcurrentAccessKeepsInvariants) {
  const TableLayout layout(kPageSize, kRowSize, kPages * kRowsPerPage);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool(layout, kFrames, file, RowLockMode::kAllocate, DirectPageTable(layout.page_count()),
            LruReplacer(kFrames), PoolHooks(), kPrefetchPages);

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

}  // namespace mvcc
