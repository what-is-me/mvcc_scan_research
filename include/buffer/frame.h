#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "common/basic_types.h"

namespace mvcc {

enum class AccessMode { kNormal, kScan };

enum class RowLockMode { kAllocate, kNone };

enum class FrameState : uint32_t { kFree, kLoading, kResident, kEvicting };

inline constexpr size_t kNoFrame = static_cast<size_t>(-1);

// 页帧：状态与 pin 由池的元数据锁串行化，dirty 由行锁保护写入、池读取。
struct Frame {
  std::atomic<uint32_t> pin{0};
  std::atomic<FrameState> state{FrameState::kFree};
  std::atomic<bool> dirty{false};
  PageId page_id = 0;
  std::span<std::mutex> row_locks;
  std::span<std::byte> data;
};

// 可作牺牲帧：驻留且未被固定。选中之后写回是在锁外做的，所以"淘汰中"的帧
// 必须自己把自己排除掉——否则另一个线程的 SelectVictim 会再选它一次，两个
// 线程同时写同一帧、各自往页表里塞不同的页。载入中的帧同理不可选。
inline bool Evictable(const Frame& frame) {
  return frame.pin.load(std::memory_order_relaxed) == 0 &&
         frame.state.load(std::memory_order_relaxed) == FrameState::kResident;
}

}  // namespace mvcc
