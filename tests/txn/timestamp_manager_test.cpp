#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
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
  Timestamp commit_ts = 0;
  std::thread committer([&] {
    const CommitScope scope(manager, 0);
    commit_ts = scope.commit_ts();
    finished.wait();
  });
  ASSERT_TRUE(gate.WaitUntilBlocked());
  ASSERT_EQ(commit_ts, Timestamp{0});

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
  EXPECT_EQ(commit_ts, Timestamp{1});
  EXPECT_EQ(observed, commit_ts);

  finish.set_value();
  committer.join();
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{2});
}

TEST(TimestampManager, ReaderDoesNotWaitForPublishedButUnfinishedCommit) {
  TimestampManager manager(1);

  const CommitScope scope(manager, 0);
  EXPECT_EQ(manager.ReadTimestamp(), Timestamp{1});
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
