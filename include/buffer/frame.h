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

}  // namespace mvcc
