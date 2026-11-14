#pragma once

#include <cstddef>
#include <span>

#include "common/basic_types.h"

namespace mvcc {

// 按页定位读写，不依赖文件指针位置。
class PageFile {
 public:
  virtual ~PageFile() = default;

  virtual void ReadPage(PageId page, std::span<std::byte> dst) = 0;
  virtual void WritePage(PageId page, std::span<std::byte> src) = 0;
};

}  // namespace mvcc
