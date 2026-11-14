#include "storage/table_layout.h"

#include <cassert>

namespace mvcc {
namespace {

size_t RoundUpToMultiple(size_t value, size_t multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

}  // namespace

TableLayout::TableLayout(size_t page_size, size_t row_size, size_t table_rows) {
  assert(row_size >= kRowHeaderSize);
  assert(row_size <= page_size);
  assert(page_size % 8 == 0);
  assert(row_size % 8 == 0);
  assert(table_rows > 0);

  page_size_ = page_size;
  row_size_ = row_size;
  rows_per_page_ = page_size / row_size;
  row_count_ = RoundUpToMultiple(table_rows, rows_per_page_);
  page_count_ = row_count_ / rows_per_page_;

  assert(rows_per_page_ <= 65536);
}

}  // namespace mvcc
