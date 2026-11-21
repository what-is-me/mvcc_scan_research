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

// 失败码必须在失败发生的下一句就存下来：拼错误消息要 to_string、分配字符串、
// 转换路径，这些都可能再动 errno / GetLastError，取晚了读到的是别人的残留值。
struct IoFailure {
  int code;
  const std::error_category* category;

  static IoFailure Capture() {
#ifdef _WIN32
    return IoFailure{static_cast<int>(::GetLastError()), &std::system_category()};
#else
    return IoFailure{errno, &std::generic_category()};
#endif
  }
};

[[noreturn]] void ThrowIoError(const IoFailure& failure, const std::string& what,
                               const std::filesystem::path& path) {
  throw std::system_error(std::error_code(failure.code, *failure.category),
                          what + ": " + path.string());
}

// 短读 / 短写不是系统错误：调用成功但没搬够字节（文件尾、设备截断）。
// 此时 errno / GetLastError 根本没被设置，拿它构造异常只会报出一个假原因。
[[noreturn]] void ThrowShortTransfer(const std::string& what, const std::filesystem::path& path,
                                     size_t done, size_t expected) {
  throw std::runtime_error(what + ": short transfer on " + path.string() + " (" +
                           std::to_string(done) + " of " + std::to_string(expected) + " bytes)");
}

void CheckBufferAlignment(std::span<const std::byte> buffer) {
  if (reinterpret_cast<uintptr_t>(buffer.data()) % kSectorBytes != 0) {
    throw std::invalid_argument("direct I/O requires a sector-aligned buffer");
  }
}

#ifndef _WIN32
// 无缓冲 I/O 不能分段重试：缓冲区地址、传输长度、文件偏移三者都必须按扇区
// 对齐，一次短传输之后 done 不再是扇区倍数，下一轮三个条件同时失配，只会
// 换回 EINVAL。所以整页请求一次发完，短传输交给调用方当硬错误报出来。
// EINTR 是唯一值得重发的失败：它一个字节都没搬走，原样重试安全。
ssize_t ReadFully(int fd, std::byte* dst, size_t bytes, uint64_t offset) {
  for (;;) {
    const ssize_t got = ::pread(fd, dst, bytes, static_cast<off_t>(offset));
    if (got >= 0 || errno != EINTR) return got;
  }
}

ssize_t WriteFully(int fd, const std::byte* src, size_t bytes, uint64_t offset) {
  for (;;) {
    const ssize_t put = ::pwrite(fd, src, bytes, static_cast<off_t>(offset));
    if (put >= 0 || errno != EINTR) return put;
  }
}
#endif

}  // namespace

void RemoveFileIfExists(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) throw std::system_error(error, "cannot remove file: " + path.string());
}

PagedFile::PagedFile(const std::filesystem::path& path, size_t page_size)
    : path_(path), page_size_(page_size) {
  if (page_size_ % kSectorBytes != 0) {
    throw std::invalid_argument("direct I/O requires a sector-aligned page size");
  }
#ifdef _WIN32
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING,
                    nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot open file", path_);
  }
  handle_ = handle;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
  if (fd < 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot open file (direct I/O unsupported by this file system?)", path_);
  }
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
  CheckBufferAlignment(dst);

  const uint64_t offset = page * page_size_;
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  const DWORD expected = static_cast<DWORD>(page_size_);
  DWORD read = 0;
  if (::ReadFile(static_cast<HANDLE>(handle_), dst.data(), expected, &read, &overlapped) == 0) {
    const IoFailure failure = IoFailure::Capture();
    // 越过文件尾在同步句柄上不是短传输而是 ERROR_HANDLE_EOF。两种平台统一
    // 报成 short transfer，调用方只需要处理一种"没读够"的语义。
    if (failure.code == ERROR_HANDLE_EOF) {
      ThrowShortTransfer("cannot read page " + std::to_string(page), path_, read, expected);
    }
    ThrowIoError(failure, "cannot read page " + std::to_string(page), path_);
  }
  if (read != expected) {
    ThrowShortTransfer("cannot read page " + std::to_string(page), path_, read, expected);
  }
#else
  const ssize_t got = ReadFully(fd_, dst.data(), page_size_, offset);
  if (got < 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot read page " + std::to_string(page), path_);
  }
  if (got != static_cast<ssize_t>(page_size_)) {
    ThrowShortTransfer("cannot read page " + std::to_string(page), path_,
                       static_cast<size_t>(got), page_size_);
  }
#endif
}

void PagedFile::ReadPages(PageId first, size_t count, std::span<std::byte> dst) {
  if (count == 0) return;
  const size_t bytes = count * page_size_;
  if (dst.size() < bytes) throw std::invalid_argument("buffer smaller than requested range");
  CheckBufferAlignment(dst);

  const uint64_t offset = first * page_size_;
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  const DWORD expected = static_cast<DWORD>(bytes);
  DWORD read = 0;
  if (::ReadFile(static_cast<HANDLE>(handle_), dst.data(), expected, &read, &overlapped) == 0) {
    const IoFailure failure = IoFailure::Capture();
    if (failure.code == ERROR_HANDLE_EOF) {
      ThrowShortTransfer("cannot read pages from " + std::to_string(first), path_, read, expected);
    }
    ThrowIoError(failure, "cannot read pages from " + std::to_string(first), path_);
  }
  if (read != expected) {
    ThrowShortTransfer("cannot read pages from " + std::to_string(first), path_, read, expected);
  }
#else
  const ssize_t got = ReadFully(fd_, dst.data(), bytes, offset);
  if (got < 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot read pages from " + std::to_string(first), path_);
  }
  if (got != static_cast<ssize_t>(bytes)) {
    ThrowShortTransfer("cannot read pages from " + std::to_string(first), path_,
                       static_cast<size_t>(got), bytes);
  }
#endif
}

void PagedFile::WritePage(PageId page, std::span<const std::byte> src) {
  if (src.size() < page_size_) throw std::invalid_argument("page buffer smaller than page size");
  CheckBufferAlignment(src);

  const uint64_t offset = page * page_size_;
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  const DWORD expected = static_cast<DWORD>(page_size_);
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(handle_), src.data(), expected, &written, &overlapped) == 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot write page " + std::to_string(page), path_);
  }
  if (written != expected) {
    ThrowShortTransfer("cannot write page " + std::to_string(page), path_, written, expected);
  }
#else
  const ssize_t put = WriteFully(fd_, src.data(), page_size_, offset);
  if (put < 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot write page " + std::to_string(page), path_);
  }
  if (put != static_cast<ssize_t>(page_size_)) {
    ThrowShortTransfer("cannot write page " + std::to_string(page), path_,
                       static_cast<size_t>(put), page_size_);
  }
#endif
}

size_t PagedFile::size_in_pages() const {
#ifdef _WIN32
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &size) == 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot query file size", path_);
  }
  return static_cast<size_t>(size.QuadPart) / page_size_;
#else
  struct stat info {};
  if (::fstat(fd_, &info) != 0) {
    const IoFailure failure = IoFailure::Capture();
    ThrowIoError(failure, "cannot query file size", path_);
  }
  return static_cast<size_t>(info.st_size) / page_size_;
#endif
}

}  // namespace mvcc
