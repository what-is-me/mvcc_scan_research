#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/basic_types.h"
#include "common/fixed_array.h"

namespace mvcc {

enum class TimestampHookPoint { kWhileReserved };

// 确定性交错注入点：生产路径为 nullptr，代价是一次可预测分支。
// 只出现在提交路径（每事务一次），读者路径不含任何钩子。
class TimestampHooks {
 public:
  TimestampHooks() = default;
  TimestampHooks(void (*notify)(void*, TimestampHookPoint), void* context)
      : notify_(notify), context_(context) {}

  void Fire(TimestampHookPoint point) const {
    if (notify_ != nullptr) notify_(context_, point);
  }

 private:
  void (*notify_)(void*, TimestampHookPoint) = nullptr;
  void* context_ = nullptr;
};

// 一个提交槽位独占一条 cache line：相邻线程的槽位不会互相踩同一行。
// 三态编码：kIdle 空闲、kReserved 已预留但尚未取号、其余为进行中的 commit_ts。
// 初值必须是 kIdle——若留成 0 就与"预留"同码，读者会把空闲槽位当成预留而死等。
struct alignas(64) TimestampSlot {
  static constexpr uint64_t kIdle = ~uint64_t{0};
  static constexpr uint64_t kReserved = 0;

  std::atomic<uint64_t> value{kIdle};
};

static_assert(sizeof(TimestampSlot) == 64);
static_assert(alignof(TimestampSlot) == 64);

// 全局提交序号与进行中提交的登记表。
//
// read_ts = min(计数器当前值, 所有进行中提交的 commit_ts)。取 min 是因为提交
// 逐行应用、逐行可见，read_ts 一旦超过某个进行中提交的 commit_ts，读者就会
// 看到它写了一半的写集。
class TimestampManager {
 public:
  static constexpr uint64_t kIdle = TimestampSlot::kIdle;
  static constexpr uint64_t kReserved = TimestampSlot::kReserved;
  static constexpr Timestamp kInitialCounter = 1;

  explicit TimestampManager(size_t slot_count, TimestampHooks hooks = {});

  TimestampManager(const TimestampManager&) = delete;
  TimestampManager& operator=(const TimestampManager&) = delete;

  size_t slot_count() const { return slots_.size(); }

  // 读快照时间戳。遇到处于预留态的槽位时等待它落定，不跳过：跳过会取到大于
  // 该提交 commit_ts 的 read_ts，而集合里又没有它，于是看到半个写集。等待先是
  // 短自旋，跨过调度边界后交给 atomic::wait，由提交方在发布 commit_ts 时唤醒。
  Timestamp ReadTimestamp() const;

  Timestamp counter() const { return counter_.load(std::memory_order_acquire); }

 private:
  friend class CommitScope;

  // 预留 -> 取号 -> 发布，返回 commit_ts。三步顺序不可调换：先取号后登记的
  // 话，两步之间的读者会取到偏大的 read_ts。
  Timestamp BeginCommit(size_t slot);
  void EndCommit(size_t slot);

  FixedArray<TimestampSlot> slots_;
  std::atomic<uint64_t> counter_{kInitialCounter};
  TimestampHooks hooks_;
};

// 提交期间持有槽位：构造时完成预留与取号，析构时释放。不提供"手动配对"的
// 两个公开函数，因为漏掉释放会让所有读者的 read_ts 永远停在被预留的那一号。
class CommitScope {
 public:
  CommitScope(TimestampManager& manager, size_t slot)
      : manager_(&manager), slot_(slot), commit_ts_(manager.BeginCommit(slot)) {}

  // 允许交出槽位所有权（池化的事务对象要把它存进来再返回），移动后源对象
  // 不再释放。移动赋值会丢掉目标已持有的槽位，因此禁止。
  CommitScope(CommitScope&& other) noexcept
      : manager_(std::exchange(other.manager_, nullptr)),
        slot_(other.slot_),
        commit_ts_(other.commit_ts_) {}

  ~CommitScope() {
    if (manager_ != nullptr) manager_->EndCommit(slot_);
  }

  CommitScope(const CommitScope&) = delete;
  CommitScope& operator=(const CommitScope&) = delete;
  CommitScope& operator=(CommitScope&&) = delete;

  Timestamp commit_ts() const { return commit_ts_; }
  size_t slot() const { return slot_; }

 private:
  TimestampManager* manager_;
  size_t slot_;
  Timestamp commit_ts_;
};

}  // namespace mvcc
