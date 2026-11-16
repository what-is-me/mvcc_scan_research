#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "common/aligned_buffer.h"
#include "storage/paged_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;

std::filesystem::path TempFile(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

void FillPage(std::span<std::byte> page, uint8_t seed) {
  for (size_t index = 0; index < page.size(); ++index) {
    page[index] = static_cast<std::byte>(seed + index % 251);
  }
}

}  // namespace

TEST(PagedFile, WritesAndReadsPageBack) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_roundtrip.tmp");
  RemoveFileIfExists(path);

  AlignedBuffer written(kPageSize);
  FillPage(written.span(), 7);
  {
    PagedFile file(path, kPageSize);
    file.WritePage(3, written.span());
    EXPECT_EQ(file.size_in_pages(), 4u);
  }

  AlignedBuffer read(kPageSize);
  {
    PagedFile file(path, kPageSize);
    file.ReadPage(3, read.span());
  }
  EXPECT_EQ(std::memcmp(written.data(), read.data(), kPageSize), 0);

  RemoveFileIfExists(path);
}

TEST(PagedFile, KeepsPagesIndependent) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_independent.tmp");
  RemoveFileIfExists(path);

  AlignedBuffer first(kPageSize);
  AlignedBuffer second(kPageSize);
  FillPage(first.span(), 11);
  FillPage(second.span(), 200);

  {
    PagedFile file(path, kPageSize);
    file.WritePage(0, first.span());
    file.WritePage(1, second.span());
  }

  {
    AlignedBuffer read(kPageSize);
    PagedFile file(path, kPageSize);
    file.ReadPage(0, read.span());
    EXPECT_EQ(std::memcmp(first.data(), read.data(), kPageSize), 0);
    file.ReadPage(1, read.span());
    EXPECT_EQ(std::memcmp(second.data(), read.data(), kPageSize), 0);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFile, RemoveFileIfExistsClearsOldContent) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_remove.tmp");
  RemoveFileIfExists(path);

  AlignedBuffer page(kPageSize);
  FillPage(page.span(), 0x5A);
  {
    PagedFile file(path, kPageSize);
    file.WritePage(0, page.span());
    EXPECT_EQ(file.size_in_pages(), 1u);
  }
  ASSERT_TRUE(std::filesystem::exists(path));

  RemoveFileIfExists(path);
  EXPECT_FALSE(std::filesystem::exists(path));

  {
    PagedFile file(path, kPageSize);
    EXPECT_EQ(file.size_in_pages(), 0u);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFile, RejectsUndersizedBuffer) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_size.tmp");
  RemoveFileIfExists(path);

  {
    PagedFile file(path, kPageSize);
    AlignedBuffer small(kPageSize - 8);
    EXPECT_THROW(file.WritePage(0, small.span()), std::invalid_argument);
    EXPECT_THROW(file.ReadPage(0, small.span()), std::invalid_argument);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFile, RejectsUnalignedBuffersAndPageSizes) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_alignment.tmp");
  RemoveFileIfExists(path);

  EXPECT_THROW(PagedFile(path, kPageSize + 512), std::invalid_argument);

  {
    PagedFile file(path, kPageSize);
    std::vector<std::byte> raw(kPageSize + kSectorBytes);
    const std::span<std::byte> misaligned = std::span(raw).subspan(1, kPageSize);
    EXPECT_THROW(file.ReadPage(0, misaligned), std::invalid_argument);
    EXPECT_THROW(file.WritePage(0, misaligned), std::invalid_argument);

    AlignedBuffer aligned(kPageSize);
    FillPage(aligned.span(), 21);
    EXPECT_NO_THROW(file.WritePage(0, aligned.span()));
    EXPECT_NO_THROW(file.ReadPage(0, aligned.span()));
  }

  RemoveFileIfExists(path);
}

}  // namespace mvcc
