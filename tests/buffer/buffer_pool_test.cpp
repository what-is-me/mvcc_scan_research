#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/arc_replacer.h"
#include "buffer/clock_replacer.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "buffer/pool_concepts.h"
#include "memory_page_file.h"
#include "storage/page.h"
#include "storage/table_layout.h"

namespace mvcc {
namespace {

static_assert(PageMapping<FrameHashTable>);
static_assert(FrameReplacer<LruReplacer>);

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kTableRows = 1000;

TableLayout TestLayout() { return TableLayout(kPageSize, kRowSize, kTableRows); }

using Pool = BufferPool<FrameHashTable, LruReplacer>;

Pool MakePool(const TableLayout& layout, size_t frame_count, PageFile& file,
              RowLockMode lock_mode = RowLockMode::kAllocate) {
  return Pool(layout, frame_count, file, lock_mode, FrameHashTable(frame_count),
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

TEST(BufferPool, FailedLoadReleasesTheFrameInsteadOfWedgingThePage) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 2, file);

  file.fail_page.store(static_cast<int64_t>(5));
  EXPECT_THROW(pool.Fix(5, AccessMode::kNormal), std::runtime_error);
  file.fail_page.store(-1);

  // 装载失败若留下"载入中"的占位，这一行会永久阻塞；这里必须能正常拿到页。
  Frame* frame = pool.Fix(5, AccessMode::kNormal);
  EXPECT_EQ(frame->page_id, PageId{5});
  pool.Unpin(frame);

  std::string error;
  EXPECT_TRUE(pool.CheckInvariants(&error)) << error;
}

TEST(BufferPool, FailedWriteBackLeavesTheFrameResident) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 1, file);

  Frame* dirty = pool.Fix(0, AccessMode::kNormal);
  std::memset(dirty->data.data(), 0xAB, layout.page_size());
  pool.MarkDirty(dirty);
  pool.Unpin(dirty);

  file.fail_write_page.store(static_cast<int64_t>(0));
  EXPECT_THROW(pool.Fix(1, AccessMode::kNormal), std::runtime_error);

  // 写回失败若把帧留在淘汰中，这一行会永久阻塞；必须退回驻留态并保留脏标志。
  Frame* recovered = pool.Fix(0, AccessMode::kNormal);
  EXPECT_EQ(recovered->page_id, PageId{0});
  EXPECT_TRUE(recovered->dirty.load(std::memory_order_relaxed));
  pool.Unpin(recovered);

  std::string error;
  EXPECT_TRUE(pool.CheckInvariants(&error)) << error;

  // 故障清除后，同一帧的写回应能正常完成。
  file.fail_write_page.store(-1);
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));
  EXPECT_EQ(file.writes.load(), 1u);
}

TEST(BufferPool, ArcReplacerKeepsInvariantsAcrossEvictions) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  BufferPool<FrameHashTable, ArcReplacer> pool(layout, 4, file, RowLockMode::kNone,
                                               FrameHashTable(4),
                                               ArcReplacer(4, layout.page_count()));

  // 顺序访问整表，4 帧装不下，每一步都在淘汰。
  for (PageId page = 0; page < layout.page_count(); ++page) {
    Frame* frame = pool.Fix(page, AccessMode::kNormal);
    ASSERT_EQ(frame->page_id, page);
    pool.Unpin(frame);
  }

  std::string error;
  EXPECT_TRUE(pool.CheckInvariants(&error)) << error;
}

// Clock 的 SelectVictim 会把热帧降冷，所以它不能当条件变量的谓词：谓词改完
// 状态返回 false 就睡死在一个再没人通知的等待上（Unpin 的通知已经发过去了，
// 而那一刻之后没有任何状态变化能再触发唤醒）。这条用例只解固一帧——若谓词
// 有副作用，工作线程会永远等下去。
TEST(BufferPool, ClockWaiterWakesOnASingleUnpin) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  using ClockPool = BufferPool<FrameHashTable, ClockReplacer>;
  ClockPool pool(layout, 2, file, RowLockMode::kNone, FrameHashTable(2), ClockReplacer(2));

  Frame* first = pool.Fix(0, AccessMode::kNormal);
  Frame* second = pool.Fix(1, AccessMode::kNormal);

  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::thread waiter([&pool, &started, &finished] {
    started.store(true);
    pool.Unpin(pool.Fix(2, AccessMode::kNormal));
    finished.store(true);
  });

  while (!started.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(finished.load());

  pool.Unpin(first);
  bool completed = false;
  for (int tick = 0; tick < 300 && !completed; ++tick) {
    if (finished.load()) {
      completed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // 失败时也要把工作线程放出来，否则 join 会把整个用例挂在这里，失败原因
  // 就只剩一个超时。
  if (!completed) pool.Unpin(second);

  waiter.join();
  EXPECT_TRUE(completed);
}

// 写回失败时替换策略不该收到任何通知：淘汰没有成立，帧应当原样留在原来的
// 层里。AbortEviction 曾经用 OnLoad 把帧"放回去"，对 ARC 那是一次幽灵命中
// ——帧从 T1 被提到 T2、p 也被改掉，返回的帧看起来一样，淘汰顺序已经变了。
// 这里用"下一次淘汰选谁"把它钉住：0 留在 T1 尾则再写它一次，被提到 T2 则
// 下一次挑中的是干净的 1，一次写都不会发生。
TEST(BufferPool, FailedWriteBackKeepsTheVictimInItsArcTier) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  using ArcPool = BufferPool<FrameHashTable, ArcReplacer>;
  ArcPool pool(layout, 3, file, RowLockMode::kNone, FrameHashTable(3),
               ArcReplacer(3, layout.page_count()));

  Frame* zero = pool.Fix(0, AccessMode::kNormal);
  pool.MarkDirty(zero);
  pool.Unpin(zero);
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));
  pool.Unpin(pool.Fix(2, AccessMode::kNormal));  // T1 = [2, 1, 0]

  // 再命中 1 一次：它升到 T2，T1 = [2, 0]，脏页 0 是 T1 尾，下一个牺牲帧。
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));

  file.fail_write_page.store(static_cast<int64_t>(0));
  EXPECT_THROW(pool.Unpin(pool.Fix(3, AccessMode::kNormal)), std::runtime_error);

  // 失败之后 0 必须还在 T1 尾：下一次淘汰应当再选它、把这次写回补上。
  // 若帧被 OnLoad 重新"装"回 T1 头，淘汰会转向 2；若被当成幽灵命中提到 T2，
  // 会转向 1。两者都干净，一次写都不会发生。
  file.fail_write_page.store(-1);
  pool.Unpin(pool.Fix(4, AccessMode::kNormal));
  EXPECT_EQ(file.writes.load(), 1u);
}

// 预取的预留循环本身会触发淘汰写回，写回失败时前面几轮已经预留的页必须原样
// 退回。它们停在"载入中"、页表条目还在，没人撤销的话之后对那一页的 Fix 会永远
// 等在一个不会成立的谓词上——清理守卫挂在循环之后就等于没有。
TEST(BufferPool, FailedPrefetchLeavesNoReservedFrames) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool(layout, 2, file, RowLockMode::kNone, FrameHashTable(2), LruReplacer(2), PoolHooks(),
            4);

  Frame* dirty = pool.Fix(0, AccessMode::kNormal);
  pool.MarkDirty(dirty);
  pool.Unpin(dirty);

  // 页 1 先占掉空闲帧，页 2 就只剩脏页 0 可淘汰——写回在这里失败。
  file.fail_write_page.store(static_cast<int64_t>(0));
  EXPECT_THROW(pool.Prefetch(1, 4, AccessMode::kScan), std::runtime_error);

  // 页 1 的预留若没被撤销，这一行会永久阻塞。
  file.fail_write_page.store(-1);
  pool.Unpin(pool.Fix(1, AccessMode::kNormal));

  std::string error;
  EXPECT_TRUE(pool.CheckInvariants(&error)) << error;
}

}  // namespace mvcc
