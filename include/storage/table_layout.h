#pragma once

#include <cstddef>

#include "common/basic_types.h"
#include "storage/row.h"

namespace mvcc {

// 定长页、定长行、无页头：槽位即物理位置，偏移 = slot * row_size。
class TableLayout {
 public:
  TableLayout(size_t page_size, size_t row_size, size_t table_rows);

  size_t page_size() const { return page_size_; }
  size_t row_size() const { return row_size_; }
  size_t data_size() const { return row_size_ - kRowHeaderSize; }
  size_t rows_per_page() const { return rows_per_page_; }
  size_t row_count() const { return row_count_; }
  size_t page_count() const { return page_count_; }
  size_t file_size() const { return page_count_ * page_size_; }

  RowId RowIdAt(size_t linear_row) const {
    return RowId::Make(linear_row / rows_per_page_,
                       static_cast<SlotId>(linear_row % rows_per_page_));
  }

  size_t LinearRow(RowId id) const { return id.page() * rows_per_page_ + id.slot(); }

 private:
  size_t page_size_ = 0;
  size_t row_size_ = 0;
  size_t rows_per_page_ = 0;
  size_t row_count_ = 0;
  size_t page_count_ = 0;
};

}  // namespace mvcc
