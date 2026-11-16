#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "common/io_mode.h"
#include "storage/table_layout.h"

namespace mvcc {

// 行数据第 word 个 8 字节字；初始化与校验共用同一推导。
uint64_t RowDataWord(size_t linear_row, size_t word_index);

// 删除旧文件后逐页写满：版本戳 0、undo_row_id 为空、数据由行号派生。
void BuildBaseFile(const std::filesystem::path& path, const TableLayout& layout,
                   IoMode mode = IoMode::kBuffered);

}  // namespace mvcc
