#include "storage/paged_file.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mvcc {
namespace {

[[noreturn]] void ThrowIoError(const std::string& what, const std::filesystem::path& path) {
  const std::string message = what + ": " + path.string();
#ifdef _WIN32
  throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), message);
#else
  throw std::system_error(errno, std::generic_category(), message);
#endif
}

}  // namespace

void RemoveFileIfExists(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) throw std::system_error(error, "cannot remove file: " + path.string());
}

PagedFile::PagedFile(const std::filesystem::path& path, size_t page_size)
    : path_(path), page_size_(page_size) {
#ifdef _WIN32
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) ThrowIoError("cannot open file", path_);
  handle_ = handle;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) ThrowIoError("cannot open file", path_);
  fd_ = fd;
#endif
}

PagedFile::~PagedFile() {
#ifdef _WIN32
  if (handle_ != nullptr) ::CloseHandle(static_cast<HANDLE>(handle_));
#else
  if (fd_ >= 0) ::close(fd_);
#endif
}

void PagedFile::ReadPage(PageId page, std::span<std::byte> dst) {
  if (dst.size() < page_size_) throw std::invalid_argument("page buffer smaller than page size");

  const uint64_t offset = page * page_size_;
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  const DWORD expected = static_cast<DWORD>(page_size_);
  DWORD read = 0;
  if (::ReadFile(static_cast<HANDLE>(handle_), dst.data(), expected, &read, &overlapped) == 0 ||
      read != expected) {
    ThrowIoError("cannot read page " + std::to_string(page), path_);
  }
#else
  size_t done = 0;
  while (done < page_size_) {
    const ssize_t got =
        ::pread(fd_, dst.data() + done, page_size_ - done, static_cast<off_t>(offset + done));
    if (got <= 0) ThrowIoError("cannot read page " + std::to_string(page), path_);
    done += static_cast<size_t>(got);
  }
#endif
}

void PagedFile::WritePage(PageId page, std::span<const std::byte> src) {
  if (src.size() < page_size_) throw std::invalid_argument("page buffer smaller than page size");

  const uint64_t offset = page * page_size_;
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  const DWORD expected = static_cast<DWORD>(page_size_);
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(handle_), src.data(), expected, &written, &overlapped) == 0 ||
      written != expected) {
    ThrowIoError("cannot write page " + std::to_string(page), path_);
  }
#else
  size_t done = 0;
  while (done < page_size_) {
    const ssize_t put =
        ::pwrite(fd_, src.data() + done, page_size_ - done, static_cast<off_t>(offset + done));
    if (put <= 0) ThrowIoError("cannot write page " + std::to_string(page), path_);
    done += static_cast<size_t>(put);
  }
#endif
}

size_t PagedFile::size_in_pages() const {
#ifdef _WIN32
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &size) == 0) {
    ThrowIoError("cannot query file size", path_);
  }
  return static_cast<size_t>(size.QuadPart) / page_size_;
#else
  struct stat info {};
  if (::fstat(fd_, &info) != 0) ThrowIoError("cannot query file size", path_);
  return static_cast<size_t>(info.st_size) / page_size_;
#endif
}

}  // namespace mvcc
