#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "storage/page.h"
#include "storage/table_layout.h"
#include "txn/undo_log.h"

#include "../buffer/memory_page_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kRowSize = 128;
constexpr size_t kTableRows = 1024;
constexpr size_t kFilePages = 256;

using UndoPool = BufferPool<FrameHashTable, LruReplacer>;

TableLayout TestLayout() { return TableLayout(kPageSize, kRowSize, kTableRows); }

UndoPool MakePool(const TableLayout& layout, size_t frame_count, PageFile& file) {
  return UndoPool(layout, frame_count, file, RowLockMode::kNone,
                  FrameHashTable(frame_count), LruReplacer(frame_count));
}

std::array<std::byte, 16> OldData(uint8_t seed) {
  std::array<std::byte, 16> data{};
  for (size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<std::byte>(seed + index);
  }
  return data;
}

TEST(UndoAppender, AllocatesFirstPageOnFirstRecord) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 8, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  {
    auto reservation = appender.Reserve(pool);
    EXPECT_EQ(reservation.id(), UndoRowId::Make(0, 0));
  }

  EXPECT_EQ(allocator.allocated(), PageId{1});
  EXPECT_EQ(appender.current_page(), PageId{0});
  EXPECT_EQ(appender.records_on_current_page(), 1u);
}

TEST(UndoAppender, FillsSlotsThenMovesToNextPage) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 8, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  const size_t total = layout.rows_per_page() + 3;
  for (size_t index = 0; index < total; ++index) {
    auto reservation = appender.Reserve(pool);
    EXPECT_EQ(reservation.id().page(), static_cast<PageId>(index / layout.rows_per_page()));
    EXPECT_EQ(reservation.id().slot(), static_cast<SlotId>(index % layout.rows_per_page()));
  }

  EXPECT_EQ(allocator.allocated(), PageId{2});
}

TEST(UndoAppender, FreshPageIsZeroFilledWithoutDeviceRead) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 8, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  {
    auto reservation = appender.Reserve(pool);
    reservation.Write(VersionStamp::Committed(3).WithLock(), UndoRowId::Make(1, 2), OldData(0));
  }

  // 新页从未落盘，必须零填充而不是读文件；读它只会拿到 EOF。
  EXPECT_EQ(file.reads.load(), 0u);
  EXPECT_EQ(file.read_ops.load(), 0u);
  EXPECT_EQ(pool.stats().physical_reads, 0u);

  Frame* frame = pool.Fix(0, AccessMode::kNormal);
  const Page page(frame->data, layout);
  for (size_t slot = 1; slot < page.row_count(); ++slot) {
    const Row row = page.RowAt(slot);
    EXPECT_EQ(row.version(), VersionStamp{});
    EXPECT_EQ(row.undo_row_id(), RowId{});
    EXPECT_EQ(std::to_integer<int>(row.data().front()), 0);
  }
  pool.Unpin(frame);
}

TEST(UndoAppender, WritesRecordWithRowLayout) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 8, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  const std::array<std::byte, 16> old_data = OldData(0x20);
  {
    auto reservation = appender.Reserve(pool);
    reservation.Write(VersionStamp::Committed(3).WithLock(), UndoRowId::Make(1, 2), old_data);
  }

  Frame* frame = pool.Fix(0, AccessMode::kNormal);
  const Row row = Page(frame->data, layout).RowAt(0);
  EXPECT_EQ(row.version(), VersionStamp::Committed(3).WithLock());
  EXPECT_EQ(row.undo_row_id(), UndoRowId::Make(1, 2));
  EXPECT_EQ(std::memcmp(row.data().data(), old_data.data(), old_data.size()), 0);
  pool.Unpin(frame);
}

TEST(UndoAppender, EvictedPageIsWrittenBackAndReadableAgain) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 1, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  for (size_t index = 0; index < layout.rows_per_page(); ++index) {
    auto reservation = appender.Reserve(pool);
    reservation.Write(VersionStamp::Committed(index + 1), RowId::Null(), OldData(1));
  }
  EXPECT_EQ(file.writes.load(), 0u);

  // 第 33 条记录需要新页，唯一那一帧被淘汰并写回。
  {
    auto reservation = appender.Reserve(pool);
    EXPECT_EQ(reservation.id().page(), PageId{1});
  }
  EXPECT_EQ(file.writes.load(), 1u);

  Frame* reloaded = pool.Fix(0, AccessMode::kNormal);
  const Page page(reloaded->data, layout);
  EXPECT_EQ(page.RowAt(0).version(), VersionStamp::Committed(1));
  EXPECT_EQ(page.RowAt(page.row_count() - 1).version(),
            VersionStamp::Committed(page.row_count()));
  pool.Unpin(reloaded);
}

TEST(UndoAppender, UnwrittenFreshPageIsWrittenBackOnEviction) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 2, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  // 首条记录因写冲突被放弃：槽位已推进，但整页一条记录都没写过。
  {
    auto reservation = appender.Reserve(pool);
    EXPECT_EQ(reservation.id().page(), PageId{0});
  }

  // 两帧都被别的页占满，page 0 是最久未用的牺牲者。
  pool.Unpin(pool.Fix(100, AccessMode::kNormal));
  pool.Unpin(pool.Fix(101, AccessMode::kNormal));

  // 它必须已经落盘。否则文件里根本没有这一页，下一次 Reserve 读它会撞上文件尾。
  EXPECT_EQ(file.writes.load(), 1u);

  Frame* reloaded = pool.Fix(0, AccessMode::kNormal);
  EXPECT_EQ(Page(reloaded->data, layout).RowAt(0).undo_row_id(), RowId{});
  pool.Unpin(reloaded);
}

// Reserve 里的 Fix 可能抛（淘汰脏帧写回失败，§7.4 明确磁盘满不是小概率）。
// 页号与槽位必须等它成功之后再推进：否则 appender 指向一个从未落盘的页号，
// 下一次 Reserve 走 Fix 读文件尾，这个线程就永久写不了 undo 了。
TEST(UndoAppender, FailedPageFixDoesNotPoisonTheAppender) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size(), test::PageFill::kZero);
  UndoPool pool = MakePool(layout, /*frame_count=*/1, file);
  UndoPageAllocator allocator;
  UndoAppender appender(allocator, layout);

  const std::array<std::byte, 16> old_data = OldData(1);
  const VersionStamp version = VersionStamp::Committed(1);

  // 把第一页写满：唯一那帧现在是脏的，下一次分配必须先把这一页写回。
  for (size_t slot = 0; slot < layout.rows_per_page(); ++slot) {
    auto reservation = appender.Reserve(pool);
    reservation.Write(version, RowId::Null(), old_data);
  }
  ASSERT_EQ(file.writes.load(), 0u);

  // 分配新页要先淘汰并写回；注入写失败让这次 Reserve 抛出。
  const PageId page_before = appender.current_page();
  const size_t slot_before = appender.records_on_current_page();
  file.fail_write_page.store(0);
  EXPECT_THROW(appender.Reserve(pool), std::runtime_error);
  file.fail_write_page.store(-1);

  // 失败的那次不能推进页/槽状态。若推进了，下一次会去 Fix 一个从未落盘的
  // 页号——真实文件读到文件尾，这个线程永久写不了 undo。内存文件不会抛，
  // 所以这里直接查状态，而不是依赖重试时的异常。
  EXPECT_EQ(appender.current_page(), page_before);
  EXPECT_EQ(appender.records_on_current_page(), slot_before);

  auto reservation = appender.Reserve(pool);
  reservation.Write(version, RowId::Null(), old_data);
  EXPECT_EQ(file.writes.load(), 1u);  // 旧页在重试时写回成功
}

TEST(UndoAppender, ConcurrentAppendersUseDistinctIds) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(kFilePages, layout.page_size());
  UndoPool pool = MakePool(layout, 32, file);
  UndoPageAllocator allocator;

  constexpr size_t kThreads = 4;
  constexpr size_t kRecords = 100;
  std::vector<std::vector<uint64_t>> ids(kThreads);

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&allocator, &layout, &pool, &ids, thread] {
      UndoAppender appender(allocator, layout);
      for (size_t index = 0; index < kRecords; ++index) {
        auto reservation = appender.Reserve(pool);
        reservation.Write(VersionStamp::Committed(index + 1), RowId::Null(), OldData(1));
        ids[thread].push_back(reservation.id().raw());
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  std::set<uint64_t> unique;
  for (const std::vector<uint64_t>& per_thread : ids) {
    unique.insert(per_thread.begin(), per_thread.end());
  }
  EXPECT_EQ(unique.size(), kThreads * kRecords);
  const size_t pages_per_thread =
      (kRecords + layout.rows_per_page() - 1) / layout.rows_per_page();
  EXPECT_EQ(allocator.allocated(), static_cast<PageId>(kThreads * pages_per_thread));
}

}  // namespace

}  // namespace mvcc
