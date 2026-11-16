#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

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

  std::vector<std::byte> written(kPageSize);
  FillPage(written, 7);
  {
    PagedFile file(path, kPageSize);
    file.WritePage(3, written);
    EXPECT_EQ(file.size_in_pages(), 4u);
  }

  std::vector<std::byte> read(kPageSize);
  {
    PagedFile file(path, kPageSize);
    file.ReadPage(3, read);
  }
  EXPECT_EQ(std::memcmp(written.data(), read.data(), kPageSize), 0);

  RemoveFileIfExists(path);
}

TEST(PagedFile, KeepsPagesIndependent) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_independent.tmp");
  RemoveFileIfExists(path);

  std::vector<std::byte> first(kPageSize);
  std::vector<std::byte> second(kPageSize);
  FillPage(first, 11);
  FillPage(second, 200);

  {
    PagedFile file(path, kPageSize);
    file.WritePage(0, first);
    file.WritePage(1, second);
  }

  std::vector<std::byte> read(kPageSize);
  {
    PagedFile file(path, kPageSize);
    file.ReadPage(0, read);
    EXPECT_EQ(std::memcmp(first.data(), read.data(), kPageSize), 0);
    file.ReadPage(1, read);
    EXPECT_EQ(std::memcmp(second.data(), read.data(), kPageSize), 0);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFile, RemoveFileIfExistsClearsOldContent) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_remove.tmp");
  RemoveFileIfExists(path);

  std::vector<std::byte> page(kPageSize, std::byte{0x5A});
  {
    PagedFile file(path, kPageSize);
    file.WritePage(0, page);
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
    std::vector<std::byte> small(kPageSize - 8);
    EXPECT_THROW(file.WritePage(0, small), std::invalid_argument);
    EXPECT_THROW(file.ReadPage(0, small), std::invalid_argument);
  }

  RemoveFileIfExists(path);
}

}  // namespace mvcc
