#pragma once

#include <cstddef>
#include <span>

#include "common/basic_types.h"

namespace mvcc {

struct RowHeader {
  VersionStamp version;
  RowId undo_row_id;
};

inline constexpr size_t kRowHeaderSize = sizeof(RowHeader);
static_assert(kRowHeaderSize == 16);

// 基表行与 undo 记录共用：16 字节头 + 定长数据区，无长度字段。
class Row {
 public:
  explicit Row(std::span<std::byte> bytes) : bytes_(bytes) {}

  RowHeader& header() const { return *reinterpret_cast<RowHeader*>(bytes_.data()); }

  VersionStamp version() const { return header().version; }
  void set_version(VersionStamp stamp) { header().version = stamp; }

  RowId undo_row_id() const { return header().undo_row_id; }
  void set_undo_row_id(RowId id) { header().undo_row_id = id; }

  std::span<std::byte> data() const { return bytes_.subspan(kRowHeaderSize); }

 private:
  std::span<std::byte> bytes_;
};

}  // namespace mvcc
