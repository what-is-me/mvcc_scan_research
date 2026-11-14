#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "buffer/page_file.h"

namespace mvcc::test {

// 每页以页号开头、其余填页号低字节，供并发测试校验读到的页身份。
class MemoryPageFile : public PageFile {
 public:
  MemoryPageFile(size_t page_count, size_t page_size)
      : page_size_(page_size), data_(page_count * page_size, std::byte{0}) {
    for (size_t page = 0; page < page_count; ++page) {
      std::byte* base = data_.data() + page * page_size_;
      std::memset(base, static_cast<int>(page & 0xFF), page_size_);
      std::memcpy(base, &page, sizeof(page));
    }
  }

  void ReadPage(PageId page, std::span<std::byte> dst) override {
    std::memcpy(dst.data(), data_.data() + page * page_size_, page_size_);
    reads.fetch_add(1, std::memory_order_relaxed);
  }

  void WritePage(PageId page, std::span<std::byte> src) override {
    std::memcpy(data_.data() + page * page_size_, src.data(), page_size_);
    writes.fetch_add(1, std::memory_order_relaxed);
  }

  std::span<std::byte> Page(PageId page) { return {data_.data() + page * page_size_, page_size_}; }

  std::atomic<size_t> reads{0};
  std::atomic<size_t> writes{0};

 private:
  size_t page_size_;
  std::vector<std::byte> data_;
};

}  // namespace mvcc::test
