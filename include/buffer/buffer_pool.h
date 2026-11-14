#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "buffer/frame.h"
#include "buffer/page_file.h"
#include "buffer/pool_hooks.h"
#include "common/fixed_array.h"
#include "storage/page.h"
#include "storage/table_layout.h"

namespace mvcc {

struct PoolStats {
  uint64_t logical_reads = 0;
  uint64_t physical_reads = 0;
  uint64_t physical_writes = 0;

  double hit_rate() const {
    if (logical_reads == 0) return 0.0;
    return 1.0 - static_cast<double>(physical_reads) / static_cast<double>(logical_reads);
  }
};

template <typename Mapping, typename Replacer>
class BufferPool {
 public:
  BufferPool(const TableLayout& layout, size_t frame_count, PageFile& file, RowLockMode lock_mode,
             Mapping mapping, Replacer replacer, PoolHooks hooks = {})
      : layout_(layout),
        frame_count_(frame_count),
        file_(file),
        table_(std::move(mapping)),
        replacer_(std::move(replacer)),
        hooks_(hooks),
        frames_(frame_count),
        row_locks_(lock_mode == RowLockMode::kAllocate ? frame_count * layout.rows_per_page() : 0),
        page_data_(frame_count * layout.page_size()) {
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

  Frame* Fix(PageId page, AccessMode mode) {
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

      if (!free_.empty()) {
        const size_t index = free_.back();
        free_.pop_back();
        table_.Insert(page, index);
        frames_[index].state.store(FrameState::kLoading, std::memory_order_relaxed);
        lock.unlock();
        return LoadInto(index, page, mode);
      }

      const std::optional<size_t> victim = replacer_.SelectVictim(frames_.span());
      if (!victim) {
        cv_.wait(lock, [this] { return replacer_.SelectVictim(frames_.span()).has_value(); });
        continue;
      }

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
      table_.Insert(page, *victim);
      victim_frame.state.store(FrameState::kLoading, std::memory_order_relaxed);
      lock.unlock();

      return LoadInto(*victim, page, mode);
    }
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

  void MarkDirty(Frame* frame) { frame->dirty.store(true, std::memory_order_release); }

  Page View(Frame* frame) const { return Page(frame->data, layout_); }

  PoolStats stats() const {
    return PoolStats{logical_reads_.load(std::memory_order_relaxed),
                     physical_reads_.load(std::memory_order_relaxed),
                     physical_writes_.load(std::memory_order_relaxed)};
  }

  size_t frame_count() const { return frame_count_; }

  size_t resident_frames() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return frame_count_ - free_.size();
  }

 private:
  bool InFlight(PageId page) const {
    const std::optional<size_t> index = table_.Find(page);
    if (!index) return false;
    return frames_[*index].state.load(std::memory_order_relaxed) != FrameState::kResident;
  }

  Frame* LoadInto(size_t index, PageId page, AccessMode mode) {
    Frame& frame = frames_[index];
    hooks_.Fire(PoolHookPoint::kBeforeFileRead);
    file_.ReadPage(page, frame.data);
    hooks_.Fire(PoolHookPoint::kAfterFileRead);
    physical_reads_.fetch_add(1, std::memory_order_relaxed);

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
  PageFile& file_;
  Mapping table_;
  Replacer replacer_;
  PoolHooks hooks_;
  FixedArray<Frame> frames_;
  FixedArray<std::mutex> row_locks_;
  FixedArray<std::byte> page_data_;
  std::vector<size_t> free_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<uint64_t> logical_reads_{0};
  std::atomic<uint64_t> physical_reads_{0};
  std::atomic<uint64_t> physical_writes_{0};
};

}  // namespace mvcc
