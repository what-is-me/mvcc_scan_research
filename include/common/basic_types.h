#pragma once

#include <cstdint>
#include <ostream>

namespace mvcc {

using PageId = uint64_t;
using SlotId = uint16_t;
using Timestamp = uint64_t;

// 最高位是锁位，低 63 位是 commit_ts；整个对象是一个 64 位字，可整体原子读写。
class VersionStamp {
 public:
  constexpr VersionStamp() = default;

  static constexpr VersionStamp Committed(Timestamp commit_ts) {
    return VersionStamp(commit_ts);
  }

  static constexpr VersionStamp FromRaw(uint64_t raw) { return VersionStamp(raw); }

  constexpr uint64_t raw() const { return raw_; }
  constexpr bool locked() const { return (raw_ & kLockBit) != 0; }
  constexpr Timestamp timestamp() const { return raw_ & kTimestampMask; }

  constexpr VersionStamp WithLock() const { return VersionStamp(raw_ | kLockBit); }

  constexpr VersionStamp WithoutLock() const { return VersionStamp(raw_ & kTimestampMask); }

  friend constexpr bool operator==(VersionStamp, VersionStamp) = default;

 private:
  explicit constexpr VersionStamp(uint64_t raw) : raw_(raw) {}

  static constexpr uint64_t kLockBit = uint64_t{1} << 63;
  static constexpr uint64_t kTimestampMask = kLockBit - 1;

  uint64_t raw_ = 0;
};

inline std::ostream& operator<<(std::ostream& out, VersionStamp stamp) {
  return out << "VersionStamp{ts=" << stamp.timestamp() << ", locked=" << std::boolalpha
             << stamp.locked() << "}";
}

// 高 48 位页号 + 低 16 位槽位；全 1 表示空。
class RowId {
 public:
  constexpr RowId() = default;

  static constexpr RowId Make(PageId page, SlotId slot) {
    return RowId((page << kSlotBits) | slot);
  }

  static constexpr RowId FromRaw(uint64_t raw) { return RowId(raw); }

  static constexpr RowId Null() { return RowId(~uint64_t{0}); }

  constexpr uint64_t raw() const { return raw_; }
  constexpr PageId page() const { return raw_ >> kSlotBits; }
  constexpr SlotId slot() const { return static_cast<SlotId>(raw_ & kSlotMask); }
  constexpr bool is_null() const { return raw_ == ~uint64_t{0}; }

  friend constexpr bool operator==(RowId, RowId) = default;

 private:
  explicit constexpr RowId(uint64_t raw) : raw_(raw) {}

  static constexpr int kSlotBits = 16;
  static constexpr uint64_t kSlotMask = (uint64_t{1} << kSlotBits) - 1;

  uint64_t raw_ = 0;
};

// 基表行与 undo 记录共用同一套定位格式。
using UndoRowId = RowId;

inline std::ostream& operator<<(std::ostream& out, RowId id) {
  if (id.is_null()) return out << "RowId{null}";
  return out << "RowId{page=" << id.page() << ", slot=" << id.slot() << "}";
}

}  // namespace mvcc
