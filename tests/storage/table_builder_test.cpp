#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "storage/page.h"
#include "storage/paged_file.h"
#include "storage/table_builder.h"
#include "storage/table_layout.h"

namespace mvcc {
namespace {

std::filesystem::path TempFile(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

uint64_t ReadWord(std::span<std::byte> data, size_t word) {
  uint64_t value = 0;
  std::memcpy(&value, data.data() + word * sizeof(value), sizeof(value));
  return value;
}

}  // namespace

TEST(TableBuilder, DerivesDistinctWords) {
  EXPECT_EQ(RowDataWord(7, 3), RowDataWord(7, 3));
  EXPECT_NE(RowDataWord(7, 3), RowDataWord(7, 4));
  EXPECT_NE(RowDataWord(7, 3), RowDataWord(8, 3));
}

TEST(TableBuilder, FillsEveryRowWithDerivedData) {
  const TableLayout layout(4096, 128, 1000);
  const std::filesystem::path path = TempFile("mvcc_build_base.tmp");
  BuildBaseFile(path, layout);

  PagedFile file(path, layout.page_size());
  ASSERT_EQ(file.size_in_pages(), layout.page_count());

  std::vector<std::byte> raw(layout.page_size());
  const size_t words_per_row = layout.data_size() / sizeof(uint64_t);
  for (const size_t page_index : {size_t{0}, layout.page_count() - 1}) {
    file.ReadPage(page_index, raw);
    const Page page(raw, layout);
    ASSERT_EQ(page.row_count(), layout.rows_per_page());

    for (size_t slot = 0; slot < page.row_count(); ++slot) {
      const size_t linear_row = page_index * layout.rows_per_page() + slot;
      const Row row = page.RowAt(slot);
      EXPECT_EQ(row.version(), VersionStamp{});
      EXPECT_FALSE(row.version().locked());
      EXPECT_EQ(row.undo_row_id(), RowId::Null());
      for (size_t word = 0; word < words_per_row; ++word) {
        ASSERT_EQ(ReadWord(row.data(), word), RowDataWord(linear_row, word))
            << "row " << linear_row << " word " << word;
      }
    }
  }

  RemoveFileIfExists(path);
}

TEST(TableBuilder, ZeroesPageTailWaste) {
  const TableLayout layout(8192, 96, 1000);
  const std::filesystem::path path = TempFile("mvcc_build_tail.tmp");
  BuildBaseFile(path, layout);

  PagedFile file(path, layout.page_size());
  std::vector<std::byte> raw(layout.page_size());
  file.ReadPage(0, raw);

  const size_t used = layout.rows_per_page() * layout.row_size();
  ASSERT_LT(used, layout.page_size());
  for (size_t offset = used; offset < layout.page_size(); ++offset) {
    ASSERT_EQ(std::to_integer<int>(raw[offset]), 0) << "offset " << offset;
  }

  RemoveFileIfExists(path);
}

TEST(TableBuilder, RebuildDropsPreviousContent) {
  const std::filesystem::path path = TempFile("mvcc_build_rebuild.tmp");

  const TableLayout wide(4096, 256, 1000);
  BuildBaseFile(path, wide);
  {
    PagedFile file(path, wide.page_size());
    EXPECT_EQ(file.size_in_pages(), wide.page_count());
  }

  const TableLayout narrow(4096, 64, 1000);
  BuildBaseFile(path, narrow);

  PagedFile file(path, narrow.page_size());
  EXPECT_EQ(file.size_in_pages(), narrow.page_count());

  std::vector<std::byte> raw(narrow.page_size());
  file.ReadPage(0, raw);
  const Page page(raw, narrow);
  EXPECT_EQ(ReadWord(page.RowAt(0).data(), 0), RowDataWord(0, 0));

  RemoveFileIfExists(path);
}

}  // namespace mvcc
