#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <span>

namespace mvcc {

// 无缓冲 I/O 要求偏移、长度与缓冲区地址对齐到的字节数。
inline constexpr size_t kSectorBytes = 4096;

// 按扇区对齐的定长字节缓冲。文件一律以无缓冲方式读写，缓冲区地址、文件
// 偏移与传输长度三者都必须对齐到扇区；页数据与建表缓冲都从这里来。
class AlignedBuffer {
 public:
  explicit AlignedBuffer(size_t bytes, size_t alignment = kSectorBytes)
      : size_(bytes),
        alignment_(alignment),
        data_(static_cast<std::byte*>(::operator new(bytes, std::align_val_t{alignment}))) {
    // 非 2 的幂在 ::operator new 的对齐重载里是未定义行为，且不会在调用点
    // 报错；断言在这里兜住。
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  }

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
