#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include "common/aligned_buffer.h"
#include "common/basic_types.h"
#include "storage/page_file.h"

namespace mvcc {

// 每次实验重建文件：先删除旧文件，不存在时忽略；删除失败抛异常，
// 避免"旧文件没删掉、新内容覆盖前面若干页、尾部残留"的静默错乱。
void RemoveFileIfExists(const std::filesystem::path& path);

// 定位读写实现：Windows 用带 OVERLAPPED 偏移的 ReadFile/WriteFile，
// 其他平台用 pread/pwrite。偏移随每次调用传入，既不读也不改文件指针，
// 所以多个线程可以对同一对象并发读写不同页。
//
// 一律以无缓冲方式打开（FILE_FLAG_NO_BUFFERING / O_DIRECT）：缓冲池是唯一
// 的页缓存，系统页缓存会让"未命中"退化成内存拷贝，使命中率统计失去意义。
// 因此页大小必须是扇区大小的整数倍，缓冲区地址也必须扇区对齐。
class PagedFile final : public PageFile {
 public:
  PagedFile(const std::filesystem::path& path, size_t page_size);
  ~PagedFile() override;

  PagedFile(const PagedFile&) = delete;
  PagedFile& operator=(const PagedFile&) = delete;

  void ReadPage(PageId page, std::span<std::byte> dst) override;
  void WritePage(PageId page, std::span<const std::byte> src) override;
  void ReadPages(PageId first, size_t count, std::span<std::byte> dst) override;

  size_t page_size() const override { return page_size_; }
  // 文件当前页数的快照；有并发写入时结果只是某一瞬间的值，仅在写入停止后有意义。
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
