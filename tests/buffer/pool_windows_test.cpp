#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <thread>

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

using Pool = BufferPool<DirectPageTable, LruReplacer>;

TableLayout TestLayout() { return TableLayout(kPageSize, kRowSize, kTableRows); }

Pool MakePool(const TableLayout& layout, size_t frame_count, PageFile& file, PoolHooks hooks) {
  return Pool(layout, frame_count, file, RowLockMode::kAllocate,
              DirectPageTable(layout.page_count()), LruReplacer(frame_count), hooks);
}

// 在指定注入点挡住执行 I/O 的线程，由测试精确决定何时放行。
class Gate {
 public:
  explicit Gate(PoolHookPoint point)
      : point_(point),
        reached_future_(reached_.get_future()),
        release_future_(release_.get_future()) {}

  PoolHooks hooks() { return PoolHooks(&Gate::Notify, this); }

  bool WaitUntilBlocked() {
    return reached_future_.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  }

  void Release() { release_.set_value(); }

 private:
  static void Notify(void* context, PoolHookPoint point) {
    auto* self = static_cast<Gate*>(context);
    if (point != self->point_ || self->fired_.exchange(true)) return;
    self->reached_.set_value();
    self->release_future_.wait();
  }

  PoolHookPoint point_;
  std::atomic<bool> fired_{false};
  std::promise<void> reached_;
  std::shared_future<void> reached_future_;
  std::promise<void> release_;
  std::shared_future<void> release_future_;
};

// 装载中的页：后到的 Fix 必须等待，并且复用同一个帧，不发生第二次读盘。
void RunInFlightLoadScenario(PoolHookPoint point) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Gate gate(point);
  Pool pool = MakePool(layout, 2, file, gate.hooks());

  Frame* first = nullptr;
  std::thread loader([&pool, &first] {
    first = pool.Fix(5, AccessMode::kNormal);
    pool.Unpin(first);
  });

  ASSERT_TRUE(gate.WaitUntilBlocked());

  std::atomic<bool> follower_done{false};
  Frame* follower_frame = nullptr;
  std::thread follower([&pool, &follower_done, &follower_frame] {
    follower_frame = pool.Fix(5, AccessMode::kNormal);
    follower_done.store(true);
    pool.Unpin(follower_frame);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(follower_done.load());

  gate.Release();
  loader.join();
  follower.join();

  EXPECT_TRUE(follower_done.load());
  EXPECT_EQ(follower_frame, first);
  EXPECT_EQ(file.reads.load(), 1u);

  size_t stored = 0;
  std::memcpy(&stored, follower_frame->data.data(), sizeof(stored));
  EXPECT_EQ(stored, 5u);
  EXPECT_EQ(follower_frame->page_id, PageId{5});
}

}  // namespace

TEST(PoolWindows, SecondFixWaitsBeforeFileRead) {
  RunInFlightLoadScenario(PoolHookPoint::kBeforeFileRead);
}

TEST(PoolWindows, SecondFixWaitsAfterFileRead) {
  RunInFlightLoadScenario(PoolHookPoint::kAfterFileRead);
}

TEST(PoolWindows, ReaderWaitsForWriteBackThenSeesNewContent) {
  const TableLayout layout = TestLayout();
  test::MemoryPageFile file(layout.page_count(), layout.page_size());
  Gate gate(PoolHookPoint::kBeforeWriteBack);
  Pool pool = MakePool(layout, 2, file, gate.hooks());

  constexpr uint64_t kMarker = 0xDEADBEEF12345678ull;
  Frame* page0 = pool.Fix(0, AccessMode::kNormal);
  std::memcpy(page0->data.data(), &kMarker, sizeof(kMarker));
  pool.MarkDirty(page0);
  pool.Unpin(page0);

  pool.Unpin(pool.Fix(9, AccessMode::kNormal));

  std::thread evictor([&pool] { pool.Unpin(pool.Fix(1, AccessMode::kNormal)); });
  ASSERT_TRUE(gate.WaitUntilBlocked());

  std::atomic<bool> reader_done{false};
  uint64_t seen = 0;
  std::thread reader([&pool, &reader_done, &seen] {
    Frame* frame = pool.Fix(0, AccessMode::kNormal);
    std::memcpy(&seen, frame->data.data(), sizeof(seen));
    reader_done.store(true);
    pool.Unpin(frame);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(reader_done.load());

  gate.Release();
  evictor.join();
  reader.join();

  EXPECT_TRUE(reader_done.load());
  EXPECT_EQ(seen, kMarker);
  EXPECT_EQ(file.writes.load(), 1u);
}

}  // namespace mvcc
