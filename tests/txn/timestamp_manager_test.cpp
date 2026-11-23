#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "txn/timestamp_manager.h"

namespace mvcc {
namespace {

static_assert(sizeof(TimestampSlot) == 64);
static_assert(alignof(TimestampSlot) == 64);

// 在"已预留、未取号"的窗口上挡住提交线程，由测试决定何时放行。
class Gate {
 public:
  explicit Gate(TimestampHookPoint point)
      : point_(point),
        reached_future_(reached_.get_future()),
        release_future_(release_.get_future()) {}

  static void Notify(void* context, TimestampHookPoint point) {
    static_cast<Gate*>(context)->OnHook(point);
  }

  TimestampHooks hooks() { return TimestampHooks(&Gate::Notify, this); }

  bool WaitUntilBlocked() {
    return reached_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void Release() { release_.set_value(); }

 private:
  void OnHook(TimestampHookPoint point) {
    if (point != point_) return;
    reached_.set_value();
    release_future_.wait();
  }

  TimestampHookPoint point_;
  std::promise<void> reached_;
  std::future<void> reached_future_;
  std::promise<void> release_;
  std::future<void> release_future_;
};

}  // namespace

TEST(TimestampManager, StartsIdleAtCounterOne) {
  TimestampManager manager(4);

  EXPECT_EQ(manager.slot_count(), 4u);
  EXPECT_EQ(manager.counter(), Timestamp{1});
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
}

void ThrowWhileReserved(void*, TimestampHookPoint) {
  throw std::runtime_error("injected hook failure");
}

TEST(TimestampManager, ThrowingHookReleasesTheReservedSlot) {
  TimestampManager timestamps(1, TimestampHooks(&ThrowWhileReserved, nullptr));

  EXPECT_THROW({ CommitScope scope(timestamps, 0); }, std::runtime_error);

  // 预留态必须撤销。停在 kReserved 的话，这里会先自旋 64 轮、再永久等待。
  EXPECT_EQ(timestamps.ReadTimestamp(), TimestampManager::kInitialCounter);
  EXPECT_EQ(timestamps.counter(), TimestampManager::kInitialCounter);
}

TEST(TimestampManager, CommitScopesAllocateConsecutiveTimestamps) {
  TimestampManager manager(2);

  for (Timestamp expected = 1; expected <= 5; ++expected) {
    CommitScope scope(manager, static_cast<size_t>(expected) % 2);
    EXPECT_EQ(scope.commit_ts(), expected);
    EXPECT_EQ(manager.ReadTimestamp(), expected);
  }

  EXPECT_EQ(manager.counter(), Timestamp{6});
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{6});
}

TEST(TimestampManager, InFlightCommitCapsReadTimestamp) {
  TimestampManager manager(2);

  CommitScope scope(manager, 0);
  ASSERT_EQ(scope.commit_ts(), Timestamp{1});

  // 已发布但未完成：read_ts 必须停在这一号，不能前进到计数器当前值。
  EXPECT_EQ(manager.counter(), Timestamp{2});
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
}

TEST(TimestampManager, TakesMinimumOfSeveralInFlightCommits) {
  TimestampManager manager(2);

  CommitScope first(manager, 0);
  CommitScope second(manager, 1);

  ASSERT_EQ(first.commit_ts(), Timestamp{1});
  ASSERT_EQ(second.commit_ts(), Timestamp{2});
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
}

TEST(TimestampManager, ReleasedCommitLiftsReadTimestamp) {
  TimestampManager manager(1);

  {
    const CommitScope scope(manager, 0);
    EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
    EXPECT_EQ(manager.counter(), Timestamp{2});
  }

  // 进行中的提交释放后，read_ts 才能前进到计数器当前值。
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{2});
}

TEST(TimestampManager, ReaderWaitsWhileSlotIsReserved) {
  Gate gate(TimestampHookPoint::kWhileReserved);
  TimestampManager manager(1, gate.hooks());

  std::promise<void> finish;
  std::future<void> finished = finish.get_future();
  // 原子化：提交线程在闸口释放之后才写入，主线程在 join 之前读它，
  // 非原子变量在这两者之间没有任何 happens-before 边（TSan 会报竞争）。
  std::atomic<Timestamp> commit_ts{0};
  std::thread committer([&] {
    const CommitScope scope(manager, 0);
    commit_ts.store(scope.commit_ts(), std::memory_order_relaxed);
    finished.wait();
  });
  ASSERT_TRUE(gate.WaitUntilBlocked());
  // 提交线程还挡在闸口，尚未取号，commit_ts 保持初值。
  ASSERT_EQ(commit_ts.load(std::memory_order_relaxed), Timestamp{0});

  std::atomic<bool> returned{false};
  Timestamp observed = 0;
  std::thread reader([&] {
    observed = manager.ReadTimestamp();
    returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_FALSE(returned.load()) << "预留态必须等待，不能跳过";

  gate.Release();
  reader.join();

  EXPECT_TRUE(returned.load());
  // 槽位停在"发布但未释放"：读者在发布点被唤醒，读到的正是刚发布的号。
  EXPECT_EQ(observed, Timestamp{1});

  finish.set_value();
  committer.join();

  // join 与提交线程建立 happens-before，读它写入的 commit_ts 才无竞争，
  // 且此刻写入必然已发生，断言不会读到初值。
  EXPECT_EQ(commit_ts.load(std::memory_order_relaxed), Timestamp{1});
  EXPECT_EQ(observed, commit_ts.load(std::memory_order_relaxed));
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{2});
}

TEST(TimestampManager, ReaderDoesNotWaitForPublishedButUnfinishedCommit) {
  TimestampManager manager(1);

  const CommitScope scope(manager, 0);
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
}

TEST(TimestampManager, WaitingReaderWakesAtPublishNotAtRelease) {
  Gate gate(TimestampHookPoint::kWhileReserved);
  TimestampManager manager(1, gate.hooks());

  std::promise<void> published;
  std::future<void> is_published = published.get_future();
  std::promise<void> finish;
  std::future<void> finished = finish.get_future();

  std::thread committer([&] {
    const CommitScope scope(manager, 0);
    published.set_value();
    // 故意让槽位停在"进行中"：读者若只在释放时被唤醒，下面的断言会超时。
    finished.wait();
  });
  ASSERT_TRUE(gate.WaitUntilBlocked());

  std::future<Timestamp> reader =
      std::async(std::launch::async, [&manager] { return manager.ReadTimestamp(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  gate.Release();
  ASSERT_EQ(is_published.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(reader.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "发布 commit_ts 后必须唤醒等待的读者，不能等到槽位释放";
  EXPECT_EQ(reader.get(), Timestamp{1});

  finish.set_value();
  committer.join();
}

TEST(TimestampManager, MovedScopeKeepsSlotUntilDestruction) {
  TimestampManager manager(1);

  {
    CommitScope original(manager, 0);
    CommitScope moved(std::move(original));
    EXPECT_EQ(moved.commit_ts(), Timestamp{1});
    EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
  }

  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{2});
}

TEST(TimestampManager, ConcurrentCommitsKeepReadTimestampBounded) {
  constexpr size_t kThreads = 4;
  constexpr size_t kIterations = 2000;

  TimestampManager manager(kThreads);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};

  std::vector<std::thread> committers;
  committers.reserve(kThreads);
  for (size_t index = 0; index < kThreads; ++index) {
    committers.emplace_back([&manager, index] {
      for (size_t iteration = 0; iteration < kIterations; ++iteration) {
        const CommitScope scope(manager, index);
        std::this_thread::yield();
      }
    });
  }

  Timestamp previous = 0;
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const Timestamp stamp = manager.ReadTimestamp();
      EXPECT_GE(stamp, Timestamp{1});
      EXPECT_GE(stamp, previous);
      EXPECT_LE(stamp, manager.counter());
      previous = stamp;
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::thread& committer : committers) committer.join();
  stop.store(true);
  reader.join();

  EXPECT_GT(reads.load(), 0u);
  EXPECT_EQ(manager.counter(), Timestamp{1 + kThreads * kIterations});
  EXPECT_EQ(manager.ReadTimestamp(), manager.counter());
}

}  // namespace mvcc
