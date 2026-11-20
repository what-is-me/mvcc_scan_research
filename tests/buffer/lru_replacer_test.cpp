#include <cstddef>
#include <optional>

#include <gtest/gtest.h>

#include "buffer/lru_replacer.h"
#include "common/fixed_array.h"

namespace mvcc {
namespace {

FixedArray<Frame> MakeFrames(size_t count) { return FixedArray<Frame>(count); }

std::optional<size_t> Victim(const LruReplacer& lru, const FixedArray<Frame>& frames) {
  return lru.SelectVictim(PageId{0}, frames.span());
}

}  // namespace

TEST(LruReplacer, PicksTheOldestFrame) {
  LruReplacer lru(3);
  const FixedArray<Frame> frames = MakeFrames(3);

  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lru.OnLoad(2, PageId{2}, AccessMode::kNormal);

  EXPECT_EQ(Victim(lru, frames), 0u);
}

TEST(LruReplacer, HitMovesFrameToFront) {
  LruReplacer lru(3);
  const FixedArray<Frame> frames = MakeFrames(3);
  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lru.OnLoad(2, PageId{2}, AccessMode::kNormal);

  lru.OnHit(0, PageId{0}, AccessMode::kNormal);

  EXPECT_EQ(Victim(lru, frames), 1u);
}

TEST(LruReplacer, ScanHitKeepsRecency) {
  LruReplacer lru(3);
  const FixedArray<Frame> frames = MakeFrames(3);
  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lru.OnLoad(2, PageId{2}, AccessMode::kNormal);

  lru.OnHit(0, PageId{0}, AccessMode::kScan);

  EXPECT_EQ(Victim(lru, frames), 0u);
}

TEST(LruReplacer, ScanLoadGoesToTheBack) {
  LruReplacer lru(2);
  const FixedArray<Frame> frames = MakeFrames(2);

  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kScan);

  EXPECT_EQ(Victim(lru, frames), 1u);
}

TEST(LruReplacer, SkipsPinnedFrames) {
  LruReplacer lru(2);
  FixedArray<Frame> frames = MakeFrames(2);
  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  frames[0].pin.store(1);

  EXPECT_EQ(Victim(lru, frames), 1u);

  frames[0].pin.store(0);
  frames[1].pin.store(1);
  EXPECT_EQ(Victim(lru, frames), 0u);
}

TEST(LruReplacer, ReportsNoVictimWhenEverythingIsPinned) {
  LruReplacer lru(2);
  FixedArray<Frame> frames = MakeFrames(2);
  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  frames[0].pin.store(1);
  frames[1].pin.store(1);

  EXPECT_FALSE(Victim(lru, frames).has_value());
}

TEST(LruReplacer, EvictedFrameLeavesTheList) {
  LruReplacer lru(2);
  const FixedArray<Frame> frames = MakeFrames(2);
  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnLoad(1, PageId{1}, AccessMode::kNormal);

  lru.OnEvict(0, PageId{0});

  EXPECT_EQ(Victim(lru, frames), 1u);
}

TEST(LruReplacer, ReloadedFrameReappearsOnce) {
  LruReplacer lru(2);
  const FixedArray<Frame> frames = MakeFrames(2);
  lru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lru.OnEvict(0, PageId{0});
  lru.OnLoad(0, PageId{0}, AccessMode::kScan);

  EXPECT_EQ(Victim(lru, frames), 0u);

  lru.OnEvict(0, PageId{0});
  EXPECT_FALSE(Victim(lru, frames).has_value());
}

}  // namespace mvcc
