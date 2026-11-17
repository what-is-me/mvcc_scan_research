#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "buffer/frame.h"
#include "buffer/page_table.h"
#include "buffer/pool_concepts.h"
#include "buffer/pool_hooks.h"
#include "common/aligned_buffer.h"
#include "common/fixed_array.h"
#include "storage/page.h"
#include "storage/page_file.h"
#include "storage/table_layout.h"

namespace mvcc {

// 基表池的默认预取深度：一次读 16 页（64 KB 页下为 1 MB）。实测再加深收益递减，
// 而单次读的范围越大，批内已驻留页造成的浪费也越多。
inline constexpr size_t kDefaultPrefetchPages = 16;

struct PoolStats {
  uint64_t logical_reads = 0;
  uint64_t physical_reads = 0;      // 从设备读入的页数（预取一批计 count 页）
  uint64_t physical_read_ops = 0;  // 实际发起的读调用次数
  uint64_t physical_writes = 0;

  double hit_rate() const {
    if (logical_reads == 0) return 0.0;
    return 1.0 - static_cast<double>(physical_reads) / static_cast<double>(logical_reads);
  }
};

template <PageMapping Mapping, FrameReplacer Replacer>
class BufferPool {
 public:
  // prefetch_pages 为 0 表示不预取（undo 池不需要顺序预取）。
  BufferPool(const TableLayout& layout, size_t frame_count, PageFile& file, RowLockMode lock_mode,
             Mapping mapping, Replacer replacer, PoolHooks hooks = {}, size_t prefetch_pages = 0)
      : layout_(layout),
        frame_count_(frame_count),
        prefetch_pages_(prefetch_pages),
        file_(file),
        table_(std::move(mapping)),
        replacer_(std::move(replacer)),
        hooks_(hooks),
        frames_(frame_count),
        row_locks_(lock_mode == RowLockMode::kAllocate ? frame_count * layout.rows_per_page() : 0),
        page_data_(frame_count * layout.page_size()),
        prefetch_buffer_(prefetch_pages * layout.page_size()) {
    for (size_t index = 0; index < frame_count_; ++index) {
      frames_[index].data =
          page_data_.span().subspan(index * layout_.page_size(), layout_.page_size());
      if (!row_locks_.empty()) {
        frames_[index].row_locks = row_locks_.span().subspan(
            index * layout_.rows_per_page(), layout_.rows_per_page());
      }
      free_.push_back(frame_count_ - 1 - index);
    }
  }

  Frame* Fix(PageId page, AccessMode mode) { return FixImpl(page, mode, LoadSource::kFile); }

  // 装入一个"逻辑上存在、但从未落盘"的页：内容全零，不发设备读。undo 文件是
  // 空文件、只追加，新分配的页在第一次写回之前根本不在文件里，读它会直接
  // 报 EOF，所以第一条记录写入前必须走这里。
  Frame* FixEmpty(PageId page, AccessMode mode) {
    return FixImpl(page, mode, LoadSource::kZeroFill);
  }

  void Unpin(Frame* frame) {
    bool became_idle = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const uint32_t previous = frame->pin.fetch_sub(1, std::memory_order_release);
      assert(previous > 0);
      became_idle = previous == 1;
    }
    if (became_idle) cv_.notify_all();
  }

  // 预取 [first, first+count) 的连续页：一次定位读装整批，把每页的往返延迟
  // 摊到 count 页上。只是提示——已驻留或在途的页跳过，装进来的页不 pin，
  // 可能在真正访问前被淘汰。返回实际装入的页数。只允许单个线程调用。
  size_t Prefetch(PageId first, size_t count, AccessMode mode) {
    if (prefetch_pages_ == 0) return 0;
    if (first >= layout_.page_count()) return 0;
    count = std::min(count, static_cast<size_t>(layout_.page_count() - first));
    count = std::min(count, prefetch_pages_);
    if (count == 0) return 0;

    const std::lock_guard<std::mutex> serialise(prefetch_mutex_);

    std::vector<size_t> reserved;
    reserved.reserve(count);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (size_t offset = 0; offset < count; ++offset) {
        if (const std::optional<size_t> index = ReserveFrame(first + offset, lock)) {
          reserved.push_back(*index);
        }
      }
    }
    if (reserved.empty()) return 0;

    const std::span<std::byte> staging = prefetch_buffer_.span().first(count * layout_.page_size());
    try {
      hooks_.Fire(PoolHookPoint::kBeforeFileRead);
      file_.ReadPages(first, count, staging);
      hooks_.Fire(PoolHookPoint::kAfterFileRead);
    } catch (...) {
      ReleaseReserved(reserved);
      throw;
    }

    physical_reads_.fetch_add(count, std::memory_order_relaxed);
    physical_read_ops_.fetch_add(1, std::memory_order_relaxed);

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const size_t index : reserved) {
        Frame& frame = frames_[index];
        const size_t offset = (frame.page_id - first) * layout_.page_size();
        std::memcpy(frame.data.data(), staging.data() + offset, layout_.page_size());
        frame.dirty.store(false, std::memory_order_relaxed);
        frame.pin.store(0, std::memory_order_relaxed);
        frame.state.store(FrameState::kResident, std::memory_order_release);
        replacer_.OnLoad(index, mode);
      }
    }
    cv_.notify_all();
    return reserved.size();
  }

  void MarkDirty(Frame* frame) { frame->dirty.store(true, std::memory_order_release); }

  Page View(Frame* frame) const { return Page(frame->data, layout_); }

  PoolStats stats() const {
    return PoolStats{logical_reads_.load(std::memory_order_relaxed),
                     physical_reads_.load(std::memory_order_relaxed),
                     physical_read_ops_.load(std::memory_order_relaxed),
                     physical_writes_.load(std::memory_order_relaxed)};
  }

  size_t frame_count() const { return frame_count_; }

  size_t resident_frames() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return frame_count_ - free_.size();
  }

  // 机械校验 §7.4 的不变量：页表与帧双向一致、一帧至多被一页引用、
  // 非驻留帧的 pin 必须为 0。只在测试里调用，生产路径不经过这里。
  bool CheckInvariants(std::string* error) const {
    const std::lock_guard<std::mutex> lock(mutex_);

    std::vector<size_t> references(frame_count_, 0);
    size_t entries = 0;
    bool ok = true;

    table_.ForEach([&](PageId page, size_t index) {
      if (!ok) return;
      ++entries;
      if (index >= frame_count_) {
        *error = "page table points past frame array";
        ok = false;
        return;
      }
      if (++references[index] > 1) {
        *error = "frame " + std::to_string(index) + " is referenced by more than one page";
        ok = false;
        return;
      }
      const Frame& frame = frames_[index];
      if (frame.page_id != page) {
        *error = "frame " + std::to_string(index) + " holds page " +
                 std::to_string(frame.page_id) + " but is mapped to page " + std::to_string(page);
        ok = false;
        return;
      }
      if (frame.state.load(std::memory_order_relaxed) == FrameState::kFree) {
        *error = "page " + std::to_string(page) + " maps to a free frame";
        ok = false;
      }
    });
    if (!ok) return false;

    if (entries != frame_count_ - free_.size()) {
      *error = "page table has " + std::to_string(entries) + " entries but " +
               std::to_string(frame_count_ - free_.size()) + " frames are out of the free list";
      return false;
    }

    for (size_t index = 0; index < frame_count_; ++index) {
      const Frame& frame = frames_[index];
      const bool resident =
          frame.state.load(std::memory_order_relaxed) == FrameState::kResident;
      if (!resident && frame.pin.load(std::memory_order_relaxed) != 0) {
        *error = "frame " + std::to_string(index) + " is pinned while not resident";
        return false;
      }
    }

    // 空闲帧必须是干净的：复用路径依赖这一点，否则上一页的脏标志会被带到新页。
    for (const size_t index : free_) {
      const Frame& frame = frames_[index];
      if (frame.state.load(std::memory_order_relaxed) != FrameState::kFree) {
        *error = "frame " + std::to_string(index) + " is in the free list but not free";
        return false;
      }
      if (frame.dirty.load(std::memory_order_relaxed)) {
        *error = "free frame " + std::to_string(index) + " is still dirty";
        return false;
      }
    }
    return true;
  }

 private:
  enum class LoadSource { kFile, kZeroFill };

  Frame* FixImpl(PageId page, AccessMode mode, LoadSource source) {
    logical_reads_.fetch_add(1, std::memory_order_relaxed);

    for (;;) {
      std::unique_lock<std::mutex> lock(mutex_);

      if (const std::optional<size_t> index = table_.Find(page)) {
        Frame& frame = frames_[*index];
        if (frame.state.load(std::memory_order_relaxed) == FrameState::kResident) {
          frame.pin.fetch_add(1, std::memory_order_relaxed);
          replacer_.OnHit(*index, mode);
          return &frame;
        }
        cv_.wait(lock, [this, page] { return !InFlight(page); });
        continue;
      }

      const std::optional<size_t> reserved = ReserveFrame(page, lock);
      if (!reserved) {
        if (table_.Find(page)) continue;
        cv_.wait(lock, [this] {
          return !free_.empty() || replacer_.SelectVictim(frames_.span()).has_value();
        });
        continue;
      }

      lock.unlock();
      return LoadInto(*reserved, page, mode, source);
    }
  }

  bool InFlight(PageId page) const {
    const std::optional<size_t> index = table_.Find(page);
    if (!index) return false;
    return frames_[*index].state.load(std::memory_order_relaxed) != FrameState::kResident;
  }

  // 在锁内为缺页的页占一个帧并写进页表，返回时帧处于 Loading，调用方负责装载。
  // 淘汰需要写回时会中途放开锁；返回 nullopt 表示该页已被别人装入，或暂时
  // 没有可淘汰的帧——两种情况都由调用方重新判断。
  std::optional<size_t> ReserveFrame(PageId page, std::unique_lock<std::mutex>& lock) {
    if (table_.Find(page)) return std::nullopt;

    if (!free_.empty()) {
      const size_t index = free_.back();
      free_.pop_back();
      assert(frames_[index].dirty.load(std::memory_order_relaxed) == false);
      table_.Insert(page, index);
      frames_[index].page_id = page;
      frames_[index].state.store(FrameState::kLoading, std::memory_order_relaxed);
      return index;
    }

    const std::optional<size_t> victim = replacer_.SelectVictim(frames_.span());
    if (!victim) return std::nullopt;

    Frame& victim_frame = frames_[*victim];
    assert(victim_frame.pin.load(std::memory_order_relaxed) == 0);
    victim_frame.state.store(FrameState::kEvicting, std::memory_order_relaxed);
    replacer_.OnEvict(*victim);
    const PageId victim_page = victim_frame.page_id;
    const bool was_dirty = victim_frame.dirty.load(std::memory_order_acquire);
    lock.unlock();

    hooks_.Fire(PoolHookPoint::kBeforeWriteBack);
    if (was_dirty) {
      file_.WritePage(victim_page, victim_frame.data);
      physical_writes_.fetch_add(1, std::memory_order_relaxed);
    }

    lock.lock();
    table_.Erase(victim_page);

    // 写回期间别的线程可能已经把这一页装进来了；此时放弃淘汰，帧退还空闲表，
    // 否则同一页会关联两个帧，帧内行锁互斥随之失效（IN2）。脏标志必须在这里
    // 清掉：内容已落盘，而空闲帧的"dirty 为假"是复用路径的前提。
    if (table_.Find(page)) {
      victim_frame.dirty.store(false, std::memory_order_relaxed);
      victim_frame.state.store(FrameState::kFree, std::memory_order_relaxed);
      free_.push_back(*victim);
      cv_.notify_all();
      return std::nullopt;
    }

    table_.Insert(page, *victim);
    victim_frame.page_id = page;
    victim_frame.dirty.store(false, std::memory_order_relaxed);
    victim_frame.pin.store(0, std::memory_order_relaxed);
    victim_frame.state.store(FrameState::kLoading, std::memory_order_relaxed);
    return *victim;
  }

  void ReleaseReserved(const std::vector<size_t>& reserved) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const size_t index : reserved) {
        table_.Erase(frames_[index].page_id);
        frames_[index].dirty.store(false, std::memory_order_relaxed);
        frames_[index].state.store(FrameState::kFree, std::memory_order_relaxed);
        free_.push_back(index);
      }
    }
    cv_.notify_all();
  }

  Frame* LoadInto(size_t index, PageId page, AccessMode mode, LoadSource source) {
    Frame& frame = frames_[index];
    if (source == LoadSource::kFile) {
      hooks_.Fire(PoolHookPoint::kBeforeFileRead);
      file_.ReadPage(page, frame.data);
      hooks_.Fire(PoolHookPoint::kAfterFileRead);
      physical_reads_.fetch_add(1, std::memory_order_relaxed);
      physical_read_ops_.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::memset(frame.data.data(), 0, frame.data.size());
    }

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      frame.page_id = page;
      frame.dirty.store(false, std::memory_order_relaxed);
      frame.pin.store(1, std::memory_order_relaxed);
      frame.state.store(FrameState::kResident, std::memory_order_release);
      replacer_.OnLoad(index, mode);
    }
    cv_.notify_all();
    return &frame;
  }

  const TableLayout& layout_;
  size_t frame_count_;
  size_t prefetch_pages_;
  PageFile& file_;
  Mapping table_;
  Replacer replacer_;
  PoolHooks hooks_;
  FixedArray<Frame> frames_;
  FixedArray<std::mutex> row_locks_;
  AlignedBuffer page_data_;
  AlignedBuffer prefetch_buffer_;
  std::vector<size_t> free_;
  mutable std::mutex mutex_;
  std::mutex prefetch_mutex_;
  std::condition_variable cv_;
  std::atomic<uint64_t> logical_reads_{0};
  std::atomic<uint64_t> physical_reads_{0};
  std::atomic<uint64_t> physical_read_ops_{0};
  std::atomic<uint64_t> physical_writes_{0};
};

}  // namespace mvcc
