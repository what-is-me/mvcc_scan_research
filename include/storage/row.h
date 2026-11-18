#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "common/basic_types.h"

namespace mvcc {

// 只描述落盘字节布局，不得用来访问内存。
struct RowHeader {
  VersionStamp version;
  RowId undo_row_id;
};

inline constexpr size_t kVersionOffset = 0;
inline constexpr size_t kUndoRowIdOffset = 8;
inline constexpr size_t kRowHeaderSize = sizeof(RowHeader);

static_assert(kRowHeaderSize == 16);
static_assert(offsetof(RowHeader, version) == kVersionOffset);
static_assert(offsetof(RowHeader, undo_row_id) == kUndoRowIdOffset);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);

// 基表行与 undo 记录共用：16 字节头 + 定长数据区，无长度字段。
//
// 头两个字会被读者在行锁之外读取（快照读），因此一律经 atomic_ref 整字读写：
// 读者与提交者并发时既没有数据竞争，也不会读到半个字段。版本戳 release 写、
// acquire 读；这条边是"看到锁位后重读 undo_row_id 必须看到新链头"的依据，不
// 只是防御性写法。不暴露 RowHeader&，避免调用方绕过原子性。
class Row {
 public:
  explicit Row(std::span<std::byte> bytes) : bytes_(bytes) {
    assert(bytes_.size() >= kRowHeaderSize);
    assert(reinterpret_cast<std::uintptr_t>(bytes_.data()) % alignof(uint64_t) == 0);
  }

  std::span<std::byte> bytes() const { return bytes_; }

  VersionStamp version() const {
    return VersionStamp::FromRaw(WordAt(kVersionOffset).load(std::memory_order_acquire));
  }

  void set_version(VersionStamp stamp) {
    WordAt(kVersionOffset).store(stamp.raw(), std::memory_order_release);
  }

  RowId undo_row_id() const {
    return RowId::FromRaw(WordAt(kUndoRowIdOffset).load(std::memory_order_relaxed));
  }

  void set_undo_row_id(RowId id) {
    WordAt(kUndoRowIdOffset).store(id.raw(), std::memory_order_relaxed);
  }

  // 数据区不加原子、不加锁，是有意的乐观读：提交线程会在读者不加锁的情况下
  // 改写它，读者可能读到改到一半的中间态。这一次读取要么被判定丢弃、要么对应
  // 提交已经写完——安全性由快照读协议保证（I7），不由这里的访问方式保证。
  // 让编译器不得把这段读推迟到版本戳之后的，是调用点的 acquire 栅栏。
  std::span<std::byte> data() const { return bytes_.subspan(kRowHeaderSize); }

 private:
  // 行头字段在页缓冲里，本身不是 uint64_t 对象，只能按字节地址取引用。对齐由
  // 布局保证（row_size 是 8 的倍数、页缓冲按扇区对齐），否则 atomic_ref 未定义。
  std::atomic_ref<uint64_t> WordAt(size_t offset) const {
    return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(bytes_.data() + offset));
  }

  std::span<std::byte> bytes_;
};

}  // namespace mvcc
