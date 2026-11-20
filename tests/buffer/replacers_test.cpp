#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/arc_replacer.h"
#include "buffer/clock_replacer.h"
#include "buffer/lru_k_replacer.h"
#include "buffer/mru_replacer.h"
#include "buffer/pool_concepts.h"

namespace mvcc {
namespace {

static_assert(FrameReplacer<ArcReplacer>);
static_assert(FrameReplacer<ClockReplacer>);
static_assert(FrameReplacer<MruReplacer>);
static_assert(FrameReplacer<LruKReplacer>);

TEST(ClockReplacer, EvictsScanLoadedPageBeforeResidentOnes) {
  std::vector<Frame> frames(3);
  ClockReplacer clock(3);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kScan);

  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(ClockReplacer, GivesHotFramesASecondChance) {
  std::vector<Frame> frames(2);
  ClockReplacer clock(2);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kNormal);

  // 第一圈把两帧都降冷，这一圈选不出；第二圈才选中最早那一帧。
  EXPECT_FALSE(clock.SelectVictim(PageId{0}, frames).has_value());
  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 0u);
}

TEST(ClockReplacer, SkipsPinnedFrames) {
  std::vector<Frame> frames(2);
  ClockReplacer clock(2);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kNormal);
  frames[0].pin.store(1);

  EXPECT_FALSE(clock.SelectVictim(PageId{0}, frames).has_value());
  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(MruReplacer, EvictsMostRecentlyLoaded) {
  std::vector<Frame> frames(3);
  MruReplacer mru(3);

  mru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  mru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  mru.OnLoad(2, PageId{2}, AccessMode::kNormal);

  EXPECT_EQ(mru.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 2u);
}

TEST(MruReplacer, HitMovesFrameToFront) {
  std::vector<Frame> frames(3);
  MruReplacer mru(3);

  mru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  mru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  mru.OnLoad(2, PageId{2}, AccessMode::kNormal);

  mru.OnHit(0, PageId{0}, AccessMode::kNormal);
  EXPECT_EQ(mru.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 0u);
}

TEST(MruReplacer, SkipsPinnedFrames) {
  std::vector<Frame> frames(3);
  MruReplacer mru(3);

  mru.OnLoad(0, PageId{0}, AccessMode::kNormal);
  mru.OnLoad(1, PageId{1}, AccessMode::kNormal);
  mru.OnLoad(2, PageId{2}, AccessMode::kNormal);
  frames[2].pin.store(1);

  EXPECT_EQ(mru.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(LruKReplacer, EvictsNeverRevisitedFirst) {
  std::vector<Frame> frames(3);
  LruKReplacer lruk(3);

  lruk.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lruk.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lruk.OnLoad(2, PageId{2}, AccessMode::kNormal);

  EXPECT_EQ(lruk.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 0u);
}

TEST(LruKReplacer, RevisitedFrameOutlivesSingleVisitOnes) {
  std::vector<Frame> frames(3);
  LruKReplacer lruk(3);

  lruk.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lruk.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lruk.OnLoad(2, PageId{2}, AccessMode::kNormal);

  lruk.OnHit(0, PageId{0}, AccessMode::kNormal);
  EXPECT_EQ(lruk.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(LruKReplacer, ScanLoadedPageGoesFirst) {
  std::vector<Frame> frames(2);
  LruKReplacer lruk(2);

  lruk.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lruk.OnLoad(1, PageId{1}, AccessMode::kScan);

  EXPECT_EQ(lruk.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(ArcReplacer, StartsByFillingT1) {
  std::vector<Frame> frames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);
  arc.OnLoad(2, PageId{2}, AccessMode::kNormal);

  // p = 0，T1 = {2,1,0}，从 T1 尾淘汰最早装入的 0。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 0u);
}

TEST(ArcReplacer, SecondHitPromotesToT2) {
  std::vector<Frame> frames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);
  arc.OnHit(0, PageId{0}, AccessMode::kNormal);  // 0 -> T2

  // T1 = {1}，T2 = {0}，p = 0：T1 > p，从 T1 淘汰 1。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 1u);
}

TEST(ArcReplacer, GhostHitOnB1MovesPageToT2) {
  std::vector<Frame> frames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{0});                      // 0 -> B1
  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);  // 幽灵命中 -> T2，p -> 1

  // T1 = {}，T2 = {0}，p = 1：T1 <= p，从 T2 淘汰 0。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 0u);
}

}  // namespace
}  // namespace mvcc
