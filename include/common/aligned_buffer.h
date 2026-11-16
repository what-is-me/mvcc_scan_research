#pragma once

#include <cstddef>
#include <new>
#include <span>

#include "common/io_mode.h"

namespace mvcc {

// 按扇区对齐的定长字节缓冲。无缓冲 I/O 要求缓冲区地址、文件偏移与长度
// 三者都对齐到扇区，页数据与建表缓冲都从这里来。
class AlignedBuffer {
 public:
  static constexpr size_t kSectorBytes = mvcc::kSectorBytes;

  explicit AlignedBuffer(size_t bytes, size_t alignment = kSectorBytes)
      : size_(bytes),
        alignment_(alignment),
        data_(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{alignment}))) {}

  ~AlignedBuffer() { ::operator delete(data_, std::align_val_t{alignment_}); }

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&&) = delete;
  AlignedBuffer& operator=(AlignedBuffer&&) = delete;

  std::span<std::byte> span() { return {data_, size_}; }
  std::span<const std::byte> span() const { return {data_, size_}; }
  std::byte* data() { return data_; }
  size_t size() const { return size_; }

 private:
  size_t size_;
  size_t alignment_;
  std::byte* data_;
};

}  // namespace mvcc
