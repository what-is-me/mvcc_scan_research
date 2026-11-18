#include "txn/timestamp_manager.h"

#include <cassert>
#include <chrono>
#include <thread>

namespace mvcc {

namespace {

// 连续这么多轮仍看到预留态，就认为持有者被调度出去了，改为短睡。正常情况下
// 预留窗口只有一次 fetch_add 的长度，第一轮 yield 之后就能继续。
constexpr int kSpinRoundsBeforeSleep = 64;

}  // namespace

TimestampManager::TimestampManager(size_t slot_count, TimestampHooks hooks)
    : slots_(slot_count), hooks_(hooks) {
  assert(slot_count > 0);
}

Timestamp TimestampManager::BeginCommit(size_t slot) {
  assert(slot < slots_.size());
  std::atomic<uint64_t>& value = slots_[slot].value;

  value.store(kReserved, std::memory_order_release);
  hooks_.Fire(TimestampHookPoint::kWhileReserved);

  const Timestamp commit_ts = counter_.fetch_add(1, std::memory_order_acq_rel);
  value.store(commit_ts, std::memory_order_release);
  return commit_ts;
}

void TimestampManager::EndCommit(size_t slot) {
  assert(slot < slots_.size());
  slots_[slot].value.store(kIdle, std::memory_order_release);
}

Timestamp TimestampManager::ReadTimestamp() const {
  int spins = 0;

  for (;;) {
    // 计数器必须先于槽位读：若本次读到的是某次取号的结果，那么该线程的"预留"
    // 写入已经 happens-before 这里的槽位读（取号是 RMW，其 release 侧与这里的
    // acquire 读构成同步），因此不可能看漏它。反过来先读槽位再读计数器，就会
    // 出现"槽位还空闲、计数器却已包含该提交"的组合。
    const Timestamp current = counter_.load(std::memory_order_acquire);
    Timestamp min_seen = current;
    bool saw_reserved = false;

    for (size_t index = 0; index < slots_.size(); ++index) {
      const uint64_t value = slots_[index].value.load(std::memory_order_acquire);
      if (value == kReserved) {
        saw_reserved = true;
        break;
      }
      if (value != kIdle && value < min_seen) min_seen = value;
    }

    if (!saw_reserved) return min_seen;

    if (++spins < kSpinRoundsBeforeSleep) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      spins = 0;
    }
  }
}

}  // namespace mvcc
