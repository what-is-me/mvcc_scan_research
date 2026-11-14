#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include "common/basic_types.h"
#include "storage/page_file.h"

namespace mvcc {

// 每次实验重建文件：先删除旧文件，不存在时忽略。
void RemoveFileIfExists(const std::filesystem::path& path);

// 定位读写实现：Windows 用带 OVERLAPPED 偏移的 ReadFile/WriteFile，
// 其他平台用 pread/pwrite，因此多个线程可以并发读写同一文件。
class PagedFile final : public PageFile {
 public:
  PagedFile(const std::filesystem::path& path, size_t page_size);
  ~PagedFile() override;

  PagedFile(const PagedFile&) = delete;
  PagedFile& operator=(const PagedFile&) = delete;

  void ReadPage(PageId page, std::span<std::byte> dst) override;
  void WritePage(PageId page, std::span<std::byte> src) override;

  size_t page_size() const { return page_size_; }
  size_t size_in_pages() const;

 private:
  std::filesystem::path path_;
  size_t page_size_ = 0;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace mvcc
