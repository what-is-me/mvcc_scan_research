#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "storage/table_layout.h"
#include "txn/row_guard.h"

#include "../buffer/memory_page_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kTableRows = 1024;

using Pool = BufferPool<FrameHashTable, LruReplacer>;

TableLayout TestLayout() { return TableLayout(kPageSize, kRowSize, kTableRows); }

Pool MakePool(const TableLayout& layout, size_t frame_count, PageFile& file) {
  return Pool(layout, frame_count, file, RowLockMode::kAllocate,
              FrameHashTable(frame_count), LruReplacer(frame_count));
}

TEST(RowGuard, HoldsTheRowUntilDestruction) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 4, file);

  std::atomic<bool> acquired{false};
  std::thread waiter;

  {
    const RowGuard guard(pool, layout, RowId::Make(0, 0));
    waiter = std::thread([&pool, &layout, &acquired] {
      const RowGuard second(pool, layout, RowId::Make(0, 0));
      acquired.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(acquired.load()) << "同一行不能被两个线程同时持有";
  }

  waiter.join();
  EXPECT_TRUE(acquired.load());
}

TEST(RowGuard, DifferentRowsOfTheSamePageDoNotBlockEachOther) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 4, file);

  const RowGuard first(pool, layout, RowId::Make(0, 0));
  const RowGuard second(pool, layout, RowId::Make(0, 1));

  EXPECT_NE(first.row().data().data(), second.row().data().data());
}

TEST(RowGuard, DifferentPagesDoNotBlockEachOther) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 4, file);

  const RowGuard first(pool, layout, RowId::Make(0, 0));
  const RowGuard second(pool, layout, RowId::Make(1, 0));

  EXPECT_NE(first.frame(), second.frame());
}

TEST(RowGuard, PinsThePageSoItCannotBeEvicted) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 1, file);

  std::atomic<bool> acquired{false};
  std::thread waiter;
  {
    const RowGuard guard(pool, layout, RowId::Make(0, 0));
    EXPECT_EQ(pool.resident_frames(), 1u);

    waiter = std::thread([&pool, &acquired] {
      pool.Unpin(pool.Fix(1, AccessMode::kNormal));
      acquired.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(acquired.load()) << "持守卫期间该帧被 pin，不能被淘汰";
  }

  waiter.join();
  EXPECT_TRUE(acquired.load());
}

TEST(RowGuard, SeesWritesMadeUnderTheGuard) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Pool pool = MakePool(layout, 4, file);

  const RowId id = RowId::Make(2, 5);
  {
    RowGuard guard(pool, layout, id);
    guard.row().set_version(VersionStamp::Committed(7).WithLock());
    guard.row().set_undo_row_id(UndoRowId::Make(3, 9));
    std::memset(guard.row().data().data(), 0xAB, guard.row().data().size());
    guard.MarkDirty();
  }

  const RowGuard reopened(pool, layout, id);
  EXPECT_EQ(reopened.row().version(), VersionStamp::Committed(7).WithLock());
  EXPECT_EQ(reopened.row().undo_row_id(), UndoRowId::Make(3, 9));
  EXPECT_EQ(std::to_integer<int>(reopened.row().data().front()), 0xAB);
}

}  // namespace

}  // namespace mvcc
