#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
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
constexpr size_t kTableRows = 1000;

TableLayout TestLayout() { return TableLayout(kPageSize, kRowSize, kTableRows); }

using Pool = BufferPool<DirectPageTable, LruReplacer>;

Pool MakePool(const TableLayout& layout, size_t frame_count, PageFile& file,
              RowLockMode lock_mode = RowLockMode::kAllocate) {
  return Pool(layout, frame_count, file, lock_mode, DirectPageTable(layout.page_count()),
              LruReplacer(frame_count));
}

RowId SlotId0(size_t slot) { return RowId::Make(0, static_cast<SlotId>(slot)); }

size_t StoredPage(const Frame* frame) {
  size_t stored = 0;
  std::memcpy(&stored, frame->data.data(), sizeof(stored));
  return stored;
}

}  // namespace

TEST(BufferPool, LoadsPageOnFirstFix) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  Frame* frame = pool.Fix(7, AccessMode::kNormal);

  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->page_id, PageId{7});
  EXPECT_EQ(frame->pin.load(), 1u);
  EXPECT_FALSE(frame->dirty.load());
  EXPECT_EQ(file.reads.load(), 1u);
  EXPECT_EQ(pool.stats().logical_reads, 1u);
  EXPECT_EQ(pool.stats().physical_reads, 1u);
  EXPECT_DOUBLE_EQ(pool.stats().hit_rate(), 0.0);
}

TEST(BufferPool, SecondFixHitsTheSameFrame) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  Frame* first = pool.Fix(7, AccessMode::kNormal);
  Frame* second = pool.Fix(7, AccessMode::kNormal);

  EXPECT_EQ(first, second);
  EXPECT_EQ(second->pin.load(), 2u);
  EXPECT_EQ(file.reads.load(), 1u);
  EXPECT_DOUBLE_EQ(pool.stats().hit_rate(), 0.5);
}

TEST(BufferPool, UnpinDropsThePinCount) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  Frame* frame = pool.Fix(0, AccessMode::kNormal);
  pool.Unpin(frame);

  EXPECT_EQ(frame->pin.load(), 0u);
  EXPECT_EQ(pool.resident_frames(), 1u);
}

TEST(BufferPool, EvictsTheLeastRecentlyUsedPage) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  pool.Unpin(pool.Fix(0, AccessMode::kNormal));
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));
  pool.Unpin(pool.Fix(0, AccessMode::kNormal));

  Frame* fresh = pool.Fix(2, AccessMode::kNormal);
  EXPECT_EQ(file.reads.load(), 3u);

  pool.Unpin(fresh);
  pool.Unpin(pool.Fix(0, AccessMode::kNormal));
  EXPECT_EQ(file.reads.load(), 3u);

  pool.Unpin(pool.Fix(1, AccessMode::kNormal));
  EXPECT_EQ(file.reads.load(), 4u);
}

TEST(BufferPool, WritesBackDirtyVictimBeforeReuse) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 1, file);

  Frame* frame = pool.Fix(0, AccessMode::kNormal);
  pool.View(frame).RowAt(2).set_undo_row_id(RowId::Make(9, 9));
  pool.MarkDirty(frame);
  pool.Unpin(frame);

  pool.Unpin(pool.Fix(1, AccessMode::kNormal));

  EXPECT_EQ(file.writes.load(), 1u);
  EXPECT_EQ(pool.stats().physical_writes, 1u);
  const Row persisted(file.Page(0).subspan(2 * layout.row_size(), layout.row_size()));
  EXPECT_EQ(persisted.undo_row_id(), RowId::Make(9, 9));
}

TEST(BufferPool, SkipsWriteBackForCleanVictim) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 1, file);

  pool.Unpin(pool.Fix(0, AccessMode::kNormal));
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));

  EXPECT_EQ(file.writes.load(), 0u);
  EXPECT_EQ(pool.stats().physical_writes, 0u);
}

TEST(BufferPool, EvictsScanLoadedPageFirst) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  pool.Unpin(pool.Fix(0, AccessMode::kNormal));
  pool.Unpin(pool.Fix(1, AccessMode::kScan));

  pool.Unpin(pool.Fix(2, AccessMode::kNormal));
  EXPECT_EQ(file.reads.load(), 3u);

  pool.Unpin(pool.Fix(1, AccessMode::kNormal));
  EXPECT_EQ(file.reads.load(), 4u);
}

TEST(BufferPool, ScanHitDoesNotRefreshRecency) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  pool.Unpin(pool.Fix(0, AccessMode::kNormal));
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));
  pool.Unpin(pool.Fix(0, AccessMode::kScan));

  pool.Unpin(pool.Fix(2, AccessMode::kNormal));
  EXPECT_EQ(file.reads.load(), 3u);

  pool.Unpin(pool.Fix(0, AccessMode::kNormal));
  EXPECT_EQ(file.reads.load(), 4u);
}

TEST(BufferPool, GivesEachFrameItsOwnRowLocks) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  Frame* first = pool.Fix(0, AccessMode::kNormal);
  Frame* second = pool.Fix(1, AccessMode::kNormal);

  EXPECT_EQ(first->row_locks.size(), layout.rows_per_page());
  EXPECT_EQ(second->row_locks.size(), layout.rows_per_page());
  EXPECT_NE(first->row_locks.data(), second->row_locks.data());
  EXPECT_EQ(first->row_locks.data() + layout.rows_per_page(), second->row_locks.data());

  first->row_locks[0].lock();
  first->row_locks[0].unlock();
}

TEST(BufferPool, SkipsRowLocksWhenDisabled) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file, RowLockMode::kNone);

  Frame* frame = pool.Fix(0, AccessMode::kNormal);

  EXPECT_TRUE(frame->row_locks.empty());
  EXPECT_EQ(pool.View(frame).row_count(), layout.rows_per_page());
  EXPECT_EQ(pool.View(frame).bytes().size(), layout.page_size());
}

TEST(BufferPool, ExposesPageViewOverFrameData) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  Frame* frame = pool.Fix(0, AccessMode::kNormal);
  Page page = pool.View(frame);
  page.RowAt(1).set_undo_row_id(SlotId0(5));

  EXPECT_EQ(page.RowAt(1).undo_row_id(), SlotId0(5));
  EXPECT_EQ(frame->data.data(), page.bytes().data());
}

TEST(BufferPool, WaitsWhenEveryFrameIsPinned) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 1, file);

  Frame* held = pool.Fix(0, AccessMode::kNormal);

  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::thread waiter([&pool, &started, &finished] {
    started.store(true);
    Frame* second = pool.Fix(1, AccessMode::kNormal);
    EXPECT_EQ(second->page_id, PageId{1});
    pool.Unpin(second);
    finished.store(true);
  });

  while (!started.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(finished.load());

  pool.Unpin(held);
  waiter.join();
  EXPECT_TRUE(finished.load());
}

TEST(BufferPool, ServesConcurrentFixAndEviction) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 8, file);

  constexpr size_t kThreads = 4;
  constexpr size_t kIterations = 3000;
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;

  for (size_t worker = 0; worker < kThreads; ++worker) {
    threads.emplace_back([&pool, &failed, &layout, worker] {
      uint64_t seed = worker * 7919 + 1;
      for (size_t iteration = 0; iteration < kIterations && !failed.load(); ++iteration) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const PageId page = static_cast<PageId>((seed >> 33) % layout.page_count());
        Frame* frame = pool.Fix(page, AccessMode::kNormal);
        if (frame->page_id != page || StoredPage(frame) != page) {
          failed.store(true);
          break;
        }
        pool.Unpin(frame);
      }
    });
  }

  for (std::thread& thread : threads) thread.join();

  EXPECT_FALSE(failed.load());
  EXPECT_GE(pool.stats().physical_reads, 1u);
}

}  // namespace mvcc
