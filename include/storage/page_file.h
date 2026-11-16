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

  virtual void ReadPage(PageId page, std::span<std::byte> dst) = 0;
  virtual void WritePage(PageId page, std::span<const std::byte> src) = 0;
};

}  // namespace mvcc
