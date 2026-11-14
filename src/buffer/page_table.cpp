#include "buffer/page_table.h"

#include <cassert>
#include <cstdint>

namespace mvcc {
namespace {

constexpr uint64_t kHashMultiplier = 0x9E3779B97F4A7C15ull;

size_t NextPowerOfTwo(size_t value) {
  size_t result = 1;
  while (result < value) result <<= 1;
  return result;
}

}  // namespace

DirectPageTable::DirectPageTable(size_t page_count) : slots_(page_count) {
  for (size_t i = 0; i < slots_.size(); ++i) slots_[i].store(kNoFrame);
}

std::optional<size_t> DirectPageTable::Find(PageId page) const {
  assert(page < slots_.size());
  const size_t frame = slots_[page].load();
  if (frame == kNoFrame) return std::nullopt;
  return frame;
}

void DirectPageTable::Insert(PageId page, size_t frame) {
  assert(page < slots_.size());
  slots_[page].store(frame);
}

void DirectPageTable::Erase(PageId page) {
  assert(page < slots_.size());
  slots_[page].store(kNoFrame);
}

FrameHashTable::FrameHashTable(size_t frame_count)
    : buckets_(NextPowerOfTwo(frame_count * 2)),
      chain_(frame_count),
      keys_(frame_count) {
  bucket_mask_ = buckets_.size() - 1;
  for (size_t i = 0; i < buckets_.size(); ++i) buckets_[i].store(kNoFrame);
  for (size_t i = 0; i < chain_.size(); ++i) chain_[i].store(kNoFrame);
  for (size_t i = 0; i < keys_.size(); ++i) keys_[i] = 0;
}

size_t FrameHashTable::BucketOf(PageId page) const {
  return static_cast<size_t>((page * kHashMultiplier) >> 32) & bucket_mask_;
}

std::optional<size_t> FrameHashTable::Find(PageId page) const {
  for (size_t frame = buckets_[BucketOf(page)].load(); frame != kNoFrame;
       frame = chain_[frame].load()) {
    if (keys_[frame] == page) return frame;
  }
  return std::nullopt;
}

void FrameHashTable::Insert(PageId page, size_t frame) {
  assert(frame < chain_.size());
  const size_t bucket = BucketOf(page);
  keys_[frame] = page;
  chain_[frame].store(buckets_[bucket].load());
  buckets_[bucket].store(frame);
}

void FrameHashTable::Erase(PageId page) {
  const size_t bucket = BucketOf(page);
  size_t previous = kNoFrame;
  for (size_t frame = buckets_[bucket].load(); frame != kNoFrame;
       frame = chain_[frame].load()) {
    if (keys_[frame] != page) {
      previous = frame;
      continue;
    }
    const size_t next = chain_[frame].load();
    if (previous == kNoFrame) {
      buckets_[bucket].store(next);
    } else {
      chain_[previous].store(next);
    }
    chain_[frame].store(kNoFrame);
    return;
  }
}

}  // namespace mvcc
