#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

  // 建表、undo 写入与测试这些单线程路径用它；并发路径不得直接读写数据区。
  std::span<std::byte> data() const { return bytes_.subspan(kRowHeaderSize); }

  // 数据区会被提交线程在读者不加锁的情况下改写，因此并发读写一律逐字原子访问：
  // 原子性消除数据竞争，relaxed 序足够——可见性由随后的版本戳 acquire 决定，
  // 读者在两者之间还要放一个 acquire 栅栏，挡住"数据读被重排到版本戳之后"。
  size_t data_words() const { return (bytes_.size() - kRowHeaderSize) / sizeof(uint64_t); }

  uint64_t data_word(size_t index) const {
    assert(index < data_words());
    return WordAt(kRowHeaderSize + index * sizeof(uint64_t)).load(std::memory_order_relaxed);
  }

  void set_data_word(size_t index, uint64_t value) {
    assert(index < data_words());
    WordAt(kRowHeaderSize + index * sizeof(uint64_t)).store(value, std::memory_order_relaxed);
  }

  void CopyDataTo(std::span<std::byte> out) const {
    assert(out.size() >= data_words() * sizeof(uint64_t));
    for (size_t index = 0; index < data_words(); ++index) {
      const uint64_t word = data_word(index);
      std::memcpy(out.data() + index * sizeof(uint64_t), &word, sizeof(word));
    }
  }

  void CopyDataFrom(std::span<const std::byte> in) {
    assert(in.size() >= data_words() * sizeof(uint64_t));
    for (size_t index = 0; index < data_words(); ++index) {
      uint64_t word = 0;
      std::memcpy(&word, in.data() + index * sizeof(uint64_t), sizeof(word));
      set_data_word(index, word);
    }
  }

 private:
  // 行头字段在页缓冲里，本身不是 uint64_t 对象，只能按字节地址取引用。对齐由
  // 布局保证（row_size 是 8 的倍数、页缓冲按扇区对齐），否则 atomic_ref 未定义。
  std::atomic_ref<uint64_t> WordAt(size_t offset) const {
    return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(bytes_.data() + offset));
  }

  std::span<std::byte> bytes_;
};

}  // namespace mvcc
