#pragma once

#include <cstddef>
#include <span>

#include "common/basic_types.h"

namespace mvcc {

// 按页定位读写。实现必须保证：不同页的并发调用互不干扰（不使用共享的文件指针）。
// 同一页的并发写不在保护范围内，由调用方（缓冲池）保证一页只有一个写者。
class PageFile {
 public:
  virtual ~PageFile() = default;

  virtual size_t page_size() const = 0;

  virtual void ReadPage(PageId page, std::span<std::byte> dst) = 0;
  virtual void WritePage(PageId page, std::span<const std::byte> src) = 0;

  // 连续读 count 页到 dst（至少 count * page_size 字节）。默认逐页读；
  // 实现合并成一次定位读时，单页的往返延迟就摊到整批上。
  virtual void ReadPages(PageId first, size_t count, std::span<std::byte> dst) {
    for (size_t offset = 0; offset < count; ++offset) {
      ReadPage(first + offset, dst.subspan(offset * page_size(), page_size()));
    }
  }
};

}  // namespace mvcc
