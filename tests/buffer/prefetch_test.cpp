#include <cstddef>
#include <cstring>

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
constexpr size_t kTableRows = 4096;
constexpr size_t kFrames = 64;
constexpr size_t kPrefetchPages = 8;

using Pool = BufferPool<DirectPageTable, LruReplacer>;

Pool MakePool(const TableLayout& layout, PageFile& file, size_t prefetch_pages) {
  return Pool(layout, kFrames, file, RowLockMode::kNone, DirectPageTable(layout.page_count()),
              LruReplacer(kFrames), PoolHooks(), prefetch_pages);
}

uint64_t FirstWord(Frame* frame) {
  uint64_t value = 0;
  std::memcpy(&value, frame->data.data(), sizeof(value));
  return value;
}

}  // namespace

TEST(Prefetch, LoadsWholeBatchInOneRead) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, file, kPrefetchPages);

  EXPECT_EQ(pool.Prefetch(0, kPrefetchPages, AccessMode::kScan), kPrefetchPages);
  EXPECT_EQ(file.read_ops.load(), 1u);
  EXPECT_EQ(file.reads.load(), kPrefetchPages);
  EXPECT_EQ(pool.resident_frames(), kPrefetchPages);

  for (size_t page = 0; page < kPrefetchPages; ++page) {
    Frame* frame = pool.Fix(page, AccessMode::kScan);
    EXPECT_EQ(FirstWord(frame), page);
    pool.Unpin(frame);
  }
  EXPECT_EQ(file.read_ops.load(), 1u);
}

TEST(Prefetch, ClampsAtTableEnd) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, file, kPrefetchPages);

  EXPECT_EQ(pool.Prefetch(layout.page_count() - 2, kPrefetchPages, AccessMode::kScan), 2u);
  EXPECT_EQ(pool.Prefetch(layout.page_count(), kPrefetchPages, AccessMode::kScan), 0u);
}

TEST(Prefetch, DisabledWhenDepthIsZero) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, file, 0);

  EXPECT_EQ(pool.Prefetch(0, kPrefetchPages, AccessMode::kScan), 0u);
  EXPECT_EQ(file.read_ops.load(), 0u);
  EXPECT_EQ(pool.resident_frames(), 0u);
}

// 预取装入、但从未被 Fix 的页会让物理读多于逻辑读；命中率若用物理读反推会
// 变成负数（1 - 8/4 = -1）。
TEST(Prefetch, HitRateCountsLogicalMissesNotPrefetchedPages) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, file, kPrefetchPages);

  ASSERT_EQ(pool.Prefetch(0, kPrefetchPages, AccessMode::kScan), kPrefetchPages);
  for (size_t page = 0; page < kPrefetchPages / 2; ++page) {
    pool.Unpin(pool.Fix(page, AccessMode::kScan));
  }

  const PoolStats stats = pool.stats();
  EXPECT_EQ(stats.logical_reads, kPrefetchPages / 2);
  EXPECT_EQ(stats.logical_misses, 0u);
  EXPECT_EQ(stats.physical_reads, kPrefetchPages);
  EXPECT_DOUBLE_EQ(stats.hit_rate(), 1.0);
}

TEST(Prefetch, KeepsResidentPagesAndStillReadsBatch) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, file, kPrefetchPages);

  pool.Unpin(pool.Fix(3, AccessMode::kNormal));
  const size_t ops_before = file.read_ops.load();

  EXPECT_EQ(pool.Prefetch(0, 4, AccessMode::kScan), 3u);
  EXPECT_EQ(file.read_ops.load(), ops_before + 1);
  EXPECT_EQ(pool.resident_frames(), 4u);

  Frame* page3 = pool.Fix(3, AccessMode::kScan);
  EXPECT_EQ(FirstWord(page3), 3u);
  pool.Unpin(page3);
}

TEST(Prefetch, PagesAreNotPinnedAndCanBeEvicted) {
  const TableLayout layout(kPageSize, kRowSize, kTableRows);
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, file, kPrefetchPages);

  pool.Prefetch(0, kPrefetchPages, AccessMode::kScan);

  for (size_t index = 0; index < kFrames; ++index) {
    const PageId page = kPrefetchPages + index;
    pool.Unpin(pool.Fix(page, AccessMode::kNormal));
  }
  EXPECT_LE(pool.resident_frames(), kFrames);
  EXPECT_GT(file.read_ops.load(), 1u);
}

}  // namespace mvcc
