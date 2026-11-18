#include "txn/timestamp_manager.h"

#include <cassert>
#include <thread>

namespace mvcc {

namespace {

// 预留窗口在正常路径上只有一次 fetch_add 的长度，先自旋几轮能把它整个留在
// 用户态；超过这个轮数说明持有者被调度出去了，再交给平台的等待原语。
constexpr int kSpinRoundsBeforeWait = 64;

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

  // 唤醒等在这个槽位上的读者。只有"离开预留态"这一步需要通知：发布之后
  // 它的 commit_ts 已经进入读者的取 min 范围，读者不再需要等它落定。
  // 没有等待者时这次通知在 Linux/Windows 上是一次空转的唤醒系统调用，
  // 相对一次提交的代价（含页 I/O）可以忽略。
  value.notify_all();
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
    const TimestampSlot* reserved = nullptr;

    for (size_t index = 0; index < slots_.size(); ++index) {
      const uint64_t value = slots_[index].value.load(std::memory_order_acquire);
      if (value == kReserved) {
        reserved = &slots_[index];
        break;
      }
      if (value != kIdle && value < min_seen) min_seen = value;
    }

    if (reserved == nullptr) return min_seen;

    if (++spins < kSpinRoundsBeforeWait) {
      std::this_thread::yield();
      continue;
    }

    // 等到这个槽位不再是"预留"：或发布出 commit_ts（此时它进入取 min 的
    // 范围，读者可以继续），或被释放回空闲。wait 自带"先比较再阻塞"，
    // 因此不会漏掉进入等待之前发生的发布。
    spins = 0;
    reserved->value.wait(kReserved, std::memory_order_acquire);
  }
}

}  // namespace mvcc
