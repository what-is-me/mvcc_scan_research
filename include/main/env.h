#pragma once

#include <cstddef>
#include <filesystem>
#include <utility>

#include "buffer/buffer_pool.h"
#include "buffer/lru_replacer.h"
#include "buffer/page_table.h"
#include "common/config.h"
#include "scan/table_scanner.h"
#include "storage/paged_file.h"
#include "storage/table_layout.h"
#include "txn/timestamp_manager.h"
#include "txn/undo_log.h"

namespace mvcc {

// §5 的 Env：一次实验的全部组件按依赖序构造，彼此只持引用。模板参数是
// 基表池的替换策略——五种策略在编译期实例化，运行期由 RunExperiment 分发；
// undo 池固定 LRU（§7.3：ARC 的幽灵表按页号稠密索引，undo 页号无上界）。
//
// 文件由调用方先备好：基表文件已建满，undo 文件为空（只追加）。
template <typename BaseReplacer>
class ExperimentEnv {
 public:
  ExperimentEnv(const Config& config, const std::filesystem::path& base_path,
                const std::filesystem::path& undo_path, BaseReplacer replacer)
      : layout(config.page_size_kb * 1024, config.row_size_b, config.table_rows),
        base_file(base_path, layout.page_size()),
        undo_file(undo_path, layout.page_size()),
        base_pool(layout, BaseFrames(config), base_file, RowLockMode::kAllocate,
                  DirectPageTable(layout.page_count()), std::move(replacer), PoolHooks(),
                  kDefaultPrefetchPages),
        undo_pool(layout, UndoFrames(config), undo_file, RowLockMode::kNone,
                  FrameHashTable(UndoFrames(config)), LruReplacer(UndoFrames(config))),
        allocator(),
        timestamps(config.tp_threads),
        scanner(base_pool, undo_pool, layout, ToAccessMode(config.scan_access_mode)) {}

  ExperimentEnv(const ExperimentEnv&) = delete;
  ExperimentEnv& operator=(const ExperimentEnv&) = delete;

  static size_t BaseFrames(const Config& config) {
    return config.base_pool_mb * 1024 * 1024 / (config.page_size_kb * 1024);
  }
  static size_t UndoFrames(const Config& config) {
    return config.undo_pool_mb * 1024 * 1024 / (config.page_size_kb * 1024);
  }

  TableLayout layout;
  PagedFile base_file;
  PagedFile undo_file;
  BufferPool<DirectPageTable, BaseReplacer> base_pool;
  BufferPool<FrameHashTable, LruReplacer> undo_pool;
  UndoPageAllocator allocator;
  TimestampManager timestamps;
  TableScanner<BufferPool<DirectPageTable, BaseReplacer>, BufferPool<FrameHashTable, LruReplacer>>
      scanner;
};

}  // namespace mvcc
