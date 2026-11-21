#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "common/basic_types.h"
#include "common/fixed_array.h"

namespace mvcc {

struct Frame;

// 事务的工作内存：read_ts + 写集 + 提交时按写集顺序 pin 住的帧。池化复用，
// 事务开始时 Reset。
//
// 写集条目只存行定位与新数据；旧值已经写进 undo 记录，不需要再缓存原
// undo_row_id——回滚不恢复链头（§9.5）。
//
// pin 数组放在这里而不是 Transaction 里：Transaction 每事务构造一次，数组
// 跟着它走就是每事务一次 malloc，违反热路径零分配（§3）。context 是池化的，
// 数组随它一次性分配、跨事务复用。
class TxnContext {
 public:
  static constexpr size_t kNoEntry = static_cast<size_t>(-1);

  TxnContext(size_t max_rows, size_t data_size)
      : max_rows_(max_rows),
        data_size_(data_size),
        entries_(max_rows),
        data_(max_rows * data_size),
        pinned_(max_rows) {}

  void Reset() {
    size_ = 0;
    read_ts_ = 0;
  }

  Timestamp read_ts() const { return read_ts_; }
  void set_read_ts(Timestamp read_ts) { read_ts_ = read_ts; }

  size_t size() const { return size_; }
  size_t capacity() const { return max_rows_; }
  size_t data_size() const { return data_size_; }
  bool full() const { return size_ >= max_rows_; }

  RowId row_at(size_t index) const {
    assert(index < size_);
    return entries_[index].row;
  }

  std::span<const std::byte> data_at(size_t index) const {
    assert(index < size_);
    return {data_.data() + index * data_size_, data_size_};
  }

  std::span<std::byte> mutable_data_at(size_t index) {
    assert(index < size_);
    return {data_.data() + index * data_size_, data_size_};
  }

  // 同一事务重复更新同一行时折叠成一条，返回已有条目下标。
  size_t Find(RowId row) const {
    for (size_t index = 0; index < size_; ++index) {
      if (entries_[index].row == row) return index;
    }
    return kNoEntry;
  }

  size_t Append(RowId row) {
    assert(!full());
    entries_[size_].row = row;
    return size_++;
  }

  // 提交时逐行 pin 住写集页，避免取号之后还要等页装载。长度等于 capacity()。
  std::span<Frame*> pinned_frames() { return pinned_.span(); }

 private:
  struct Entry {
    RowId row;
  };

  size_t max_rows_;
  size_t data_size_;
  FixedArray<Entry> entries_;
  std::vector<std::byte> data_;
  FixedArray<Frame*> pinned_;
  size_t size_ = 0;
  Timestamp read_ts_ = 0;
};

}  // namespace mvcc
