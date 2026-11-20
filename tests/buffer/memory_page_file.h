#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include "storage/page_file.h"

namespace mvcc::test {

enum class PageFill {
  kPageId,  // 每页以页号开头、其余填页号低字节，供并发测试校验读到的页身份
  kZero,    // 全零，模拟按真实建表规则初始化过的基表
};

class MemoryPageFile : public PageFile {
 public:
  MemoryPageFile(size_t page_count, size_t page_size, PageFill fill = PageFill::kPageId)
      : page_size_(page_size), data_(page_count * page_size, std::byte{0}) {
    if (fill == PageFill::kZero) return;
    for (size_t page = 0; page < page_count; ++page) {
      std::byte* base = data_.data() + page * page_size_;
      std::memset(base, static_cast<int>(page & 0xFF), page_size_);
      std::memcpy(base, &page, sizeof(page));
    }
  }

  void ReadPage(PageId page, std::span<std::byte> dst) override {
    if (fail_page.load(std::memory_order_relaxed) == static_cast<int64_t>(page)) {
      throw std::runtime_error("injected read failure");
    }
    std::memcpy(dst.data(), data_.data() + page * page_size_, page_size_);
    reads.fetch_add(1, std::memory_order_relaxed);
    read_ops.fetch_add(1, std::memory_order_relaxed);
  }

  void ReadPages(PageId first, size_t count, std::span<std::byte> dst) override {
    // 批量读也要能注入失败：预取的中途失败会走 ReleaseReserved，那条路径
    // 只有从整批里挑一页弄坏才测得到。
    const int64_t failed = fail_page.load(std::memory_order_relaxed);
    if (failed >= 0 && static_cast<size_t>(failed) >= first &&
        static_cast<size_t>(failed) < first + count) {
      throw std::runtime_error("injected batch read failure");
    }
    std::memcpy(dst.data(), data_.data() + first * page_size_, count * page_size_);
    reads.fetch_add(count, std::memory_order_relaxed);
    read_ops.fetch_add(1, std::memory_order_relaxed);
  }

  void WritePage(PageId page, std::span<const std::byte> src) override {
    if (fail_write_page.load(std::memory_order_relaxed) == static_cast<int64_t>(page)) {
      throw std::runtime_error("injected write failure");
    }
    std::memcpy(data_.data() + page * page_size_, src.data(), page_size_);
    writes.fetch_add(1, std::memory_order_relaxed);
  }

  size_t page_size() const override { return page_size_; }

  std::span<std::byte> Page(PageId page) { return {data_.data() + page * page_size_, page_size_}; }

  std::atomic<size_t> reads{0};
  std::atomic<size_t> read_ops{0};
  std::atomic<size_t> writes{0};

  // 读该页即抛异常，用于验证装载失败 / 提交失败不会留下半成品状态。
  std::atomic<int64_t> fail_page{-1};

  // 写该页即抛异常，用于验证写回失败不会把帧永久留在淘汰中。
  std::atomic<int64_t> fail_write_page{-1};

 private:
  size_t page_size_;
  std::vector<std::byte> data_;
};

}  // namespace mvcc::test
