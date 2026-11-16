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

TEST(PagedFile, DirectModeRoundTripsSectorAlignedPages) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_direct.tmp");
  RemoveFileIfExists(path);

  constexpr size_t kBigPage = 64 * 1024;
  AlignedBuffer written(kBigPage);
  FillPage(written.span(), 13);
  {
    PagedFile file(path, kBigPage, IoMode::kDirect);
    file.WritePage(2, written.span());
    EXPECT_EQ(file.size_in_pages(), 3u);
  }

  {
    AlignedBuffer read(kBigPage);
    PagedFile file(path, kBigPage, IoMode::kDirect);
    file.ReadPage(2, read.span());
    EXPECT_EQ(std::memcmp(written.data(), read.data(), kBigPage), 0);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFile, DirectModeRejectsUnalignedBuffersAndPageSizes) {
  const std::filesystem::path path = TempFile("mvcc_paged_file_direct_guard.tmp");
  RemoveFileIfExists(path);

  EXPECT_THROW(PagedFile(path, kPageSize + 512, IoMode::kDirect), std::invalid_argument);

  {
    PagedFile file(path, kPageSize, IoMode::kDirect);
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
