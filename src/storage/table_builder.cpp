#include "storage/table_builder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "common/aligned_buffer.h"
#include "storage/page.h"
#include "storage/paged_file.h"

namespace mvcc {
namespace {

uint64_t Mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

}  // namespace

uint64_t RowDataWord(size_t linear_row, size_t word_index) {
  return Mix(static_cast<uint64_t>(linear_row) * 0x9E3779B97F4A7C15ull +
             static_cast<uint64_t>(word_index) * 0xBF58476D1CE4E5B9ull);
}

void BuildBaseFile(const std::filesystem::path& path, const TableLayout& layout, IoMode mode) {
  RemoveFileIfExists(path);

  PagedFile file(path, layout.page_size(), mode);
  AlignedBuffer buffer(layout.page_size());
  std::fill(buffer.span().begin(), buffer.span().end(), std::byte{0});

  const size_t words_per_row = layout.data_size() / sizeof(uint64_t);
  for (size_t page_index = 0; page_index < layout.page_count(); ++page_index) {
    const Page page(buffer.span(), layout);
    for (size_t slot = 0; slot < page.row_count(); ++slot) {
      const size_t linear_row = page_index * layout.rows_per_page() + slot;
      Row row = page.RowAt(slot);
      row.set_version(VersionStamp{});
      row.set_undo_row_id(RowId::Null());

      std::byte* data = row.data().data();
      for (size_t word = 0; word < words_per_row; ++word) {
        const uint64_t value = RowDataWord(linear_row, word);
        std::memcpy(data + word * sizeof(value), &value, sizeof(value));
      }
    }
    file.WritePage(static_cast<PageId>(page_index), buffer.span());
  }

  const size_t written = file.size_in_pages();
  if (written != layout.page_count()) {
    throw std::runtime_error("base file has " + std::to_string(written) + " pages, expected " +
                             std::to_string(layout.page_count()));
  }
}

}  // namespace mvcc
