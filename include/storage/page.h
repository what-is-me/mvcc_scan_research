#pragma once

#include <cstddef>
#include <span>

#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {

// 页缓冲区的视图：无页头，槽位即偏移 slot * row_size。
class Page {
 public:
  Page(std::span<std::byte> bytes, const TableLayout& layout)
      : bytes_(bytes), row_size_(layout.row_size()), row_count_(layout.rows_per_page()) {}

  std::span<std::byte> bytes() const { return bytes_; }
  size_t row_count() const { return row_count_; }

  Row RowAt(size_t slot) const { return Row(bytes_.subspan(slot * row_size_, row_size_)); }

 private:
  std::span<std::byte> bytes_;
  size_t row_size_;
  size_t row_count_;
};

}  // namespace mvcc
