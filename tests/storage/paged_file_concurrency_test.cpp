#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "storage/paged_file.h"

namespace mvcc {
namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kThreads = 4;
constexpr size_t kPagesPerThread = 250;
constexpr size_t kTotalPages = kThreads * kPagesPerThread;

std::filesystem::path TempFile(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

void FillForPage(std::span<std::byte> page, size_t page_index) {
  std::memset(page.data(), static_cast<int>(page_index & 0xFF), page.size());
  std::memcpy(page.data(), &page_index, sizeof(page_index));
}

bool MatchesPage(std::span<const std::byte> page, size_t page_index) {
  size_t stored = 0;
  std::memcpy(&stored, page.data(), sizeof(stored));
  if (stored != page_index) return false;
  for (size_t index = sizeof(stored); index < page.size(); ++index) {
    if (page[index] != static_cast<std::byte>(page_index & 0xFF)) return false;
  }
  return true;
}

void WriteAllPagesConcurrently(PagedFile& file) {
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  for (size_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&file, &next] {
      std::vector<std::byte> buffer(kPageSize);
      for (size_t index = 0; index < kPagesPerThread; ++index) {
        const size_t page = next.fetch_add(1);
        FillForPage(buffer, page);
        file.WritePage(page, buffer);
      }
    });
  }
  for (auto& worker : workers) worker.join();
}

}  // namespace

TEST(PagedFileConcurrency, ConcurrentWritesToDistinctPagesStayIntact) {
  const std::filesystem::path path = TempFile("mvcc_paged_concurrent_write.tmp");
  RemoveFileIfExists(path);

  {
    PagedFile file(path, kPageSize);
    WriteAllPagesConcurrently(file);
    EXPECT_EQ(file.size_in_pages(), kTotalPages);
  }

  {
    PagedFile file(path, kPageSize);
    std::vector<std::byte> buffer(kPageSize);
    size_t corrupted = 0;
    for (size_t page = 0; page < kTotalPages; ++page) {
      file.ReadPage(page, buffer);
      if (!MatchesPage(buffer, page)) ++corrupted;
    }
    EXPECT_EQ(corrupted, 0u);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFileConcurrency, ConcurrentReadsSeeCompletePages) {
  const std::filesystem::path path = TempFile("mvcc_paged_concurrent_read.tmp");
  RemoveFileIfExists(path);

  {
    PagedFile file(path, kPageSize);
    WriteAllPagesConcurrently(file);
  }

  {
    PagedFile file(path, kPageSize);
    std::atomic<size_t> next{0};
    std::atomic<size_t> corrupted{0};
    std::vector<std::thread> readers;
    for (size_t thread = 0; thread < kThreads; ++thread) {
      readers.emplace_back([&file, &next, &corrupted] {
        std::vector<std::byte> buffer(kPageSize);
        for (;;) {
          const size_t page = next.fetch_add(1);
          if (page >= kTotalPages) break;
          file.ReadPage(page, buffer);
          if (!MatchesPage(buffer, page)) corrupted.fetch_add(1);
        }
      });
    }
    for (auto& reader : readers) reader.join();

    EXPECT_EQ(corrupted.load(), 0u);
  }

  RemoveFileIfExists(path);
}

TEST(PagedFileConcurrency, ConcurrentAppendLeavesNoHole) {
  const std::filesystem::path path = TempFile("mvcc_paged_concurrent_append.tmp");
  RemoveFileIfExists(path);

  {
    PagedFile file(path, kPageSize);
    WriteAllPagesConcurrently(file);

    EXPECT_EQ(file.size_in_pages(), kTotalPages);
    EXPECT_EQ(std::filesystem::file_size(path), kTotalPages * kPageSize);
  }

  RemoveFileIfExists(path);
}

}  // namespace mvcc
