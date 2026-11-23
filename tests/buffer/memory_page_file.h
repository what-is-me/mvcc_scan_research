#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
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
      : page_count_(page_count),
        page_size_(page_size),
        data_(page_count * page_size, std::byte{0}) {
    if (fill == PageFill::kZero) return;
    for (size_t page = 0; page < page_count; ++page) {
      std::byte* base = data_.data() + page * page_size_;
      std::memset(base, static_cast<int>(page & 0xFF), page_size_);
      std::memcpy(base, &page, sizeof(page));
    }
  }

  // 边界语义对齐真实 PageFile：越界页号是硬错误，必须明确报出而不是静默
  // 越界踩堆。否则配置错的测试（undo 容量小于满跑负载）会以内存踩踏呈现
  // ——偶发 SEGFAULT，Debug 断言都抓不到。
  void CheckRange(PageId page) const {
    if (static_cast<size_t>(page) >= page_count_) {
      throw std::runtime_error("MemoryPageFile: page " + std::to_string(page) +
                               " is beyond the file's " + std::to_string(page_count_) +
                               " pages (test resource too small)");
    }
  }

  // 真实文件对同一区域的并发 I/O 由内核串行化（ext4 DIO 的 inode 锁：读
  // 共享、写排他，读取不会撕裂）；替身没有内核，自己补一把文件级 mutex，
  // 语义与 TSan 观察都对齐——预取整批设备读（含对已驻留页的重复读）与
  // 淘汰写回会并发触碰同一份 data_。锁只包住替身自己的内存拷贝，调用方
  // （池）本来就在池锁之外发 I/O，无锁序交互。
  void ReadPage(PageId page, std::span<std::byte> dst) override {
    const std::lock_guard<std::mutex> guard(io_mutex_);
    if (fail_page.load(std::memory_order_relaxed) == static_cast<int64_t>(page)) {
      throw std::runtime_error("injected read failure");
    }
    CheckRange(page);
    std::memcpy(dst.data(), data_.data() + page * page_size_, page_size_);
    reads.fetch_add(1, std::memory_order_relaxed);
    read_ops.fetch_add(1, std::memory_order_relaxed);
  }

  void ReadPages(PageId first, size_t count, std::span<std::byte> dst) override {
    const std::lock_guard<std::mutex> guard(io_mutex_);
    // 批量读也要能注入失败：预取的中途失败会走 ReleaseReserved，那条路径
    // 只有从整批里挑一页弄坏才测得到。
    const int64_t failed = fail_page.load(std::memory_order_relaxed);
    if (failed >= 0 && static_cast<size_t>(failed) >= first &&
        static_cast<size_t>(failed) < first + count) {
      throw std::runtime_error("injected batch read failure");
    }
    CheckRange(first);
    CheckRange(first + count - 1);
    std::memcpy(dst.data(), data_.data() + first * page_size_, count * page_size_);
    reads.fetch_add(count, std::memory_order_relaxed);
    read_ops.fetch_add(1, std::memory_order_relaxed);
  }

  void WritePage(PageId page, std::span<const std::byte> src) override {
    const std::lock_guard<std::mutex> guard(io_mutex_);
    if (fail_write_page.load(std::memory_order_relaxed) == static_cast<int64_t>(page)) {
      throw std::runtime_error("injected write failure");
    }
    CheckRange(page);
    std::memcpy(data_.data() + page * page_size_, src.data(), page_size_);
    writes.fetch_add(1, std::memory_order_relaxed);
  }

  size_t page_size() const override { return page_size_; }

  // 不加锁的直接访问：构造后按页填身份、测试里单线程读取持久化结果，
  // 都不在并发路径上。
  std::span<std::byte> Page(PageId page) {
    CheckRange(page);
    return {data_.data() + page * page_size_, page_size_};
  }

  std::atomic<size_t> reads{0};
  std::atomic<size_t> read_ops{0};
  std::atomic<size_t> writes{0};

  // 读该页即抛异常，用于验证装载失败 / 提交失败不会留下半成品状态。
  std::atomic<int64_t> fail_page{-1};

  // 写该页即抛异常，用于验证写回失败不会把帧永久留在淘汰中。
  std::atomic<int64_t> fail_write_page{-1};

 private:
  size_t page_count_;
  size_t page_size_;
  mutable std::mutex io_mutex_;
  std::vector<std::byte> data_;
};

}  // namespace mvcc::test
