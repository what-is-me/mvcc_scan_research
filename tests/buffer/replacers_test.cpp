#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "buffer/arc_replacer.h"
#include "buffer/clock_replacer.h"
#include "buffer/lru_k_replacer.h"
#include "buffer/pool_concepts.h"

namespace mvcc {
namespace {

// 测试里显式把帧置为驻留态：可淘汰的判定是"驻留且 pin = 0"，默认构造的帧
// 是空闲态，任何策略都不该选它。
std::vector<Frame> ResidentFrames(size_t count) {
  std::vector<Frame> frames(count);
  for (Frame& frame : frames) {
    frame.state.store(FrameState::kResident, std::memory_order_relaxed);
  }
  return frames;
}

static_assert(FrameReplacer<ArcReplacer>);
static_assert(FrameReplacer<ClockReplacer>);
static_assert(FrameReplacer<LruKReplacer>);

TEST(ClockReplacer, EvictsScanLoadedPageBeforeResidentOnes) {
  std::vector<Frame> frames = ResidentFrames(3);
  ClockReplacer clock(3);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kScan);

  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(ClockReplacer, GivesHotFramesASecondChance) {
  std::vector<Frame> frames = ResidentFrames(2);
  ClockReplacer clock(2);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kNormal);

  // 第一圈把两帧都降冷，这一圈选不出；第二圈才选中最早那一帧。
  EXPECT_FALSE(clock.SelectVictim(PageId{0}, frames).has_value());
  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 0u);
}

TEST(ClockReplacer, SkipsPinnedFrames) {
  std::vector<Frame> frames = ResidentFrames(2);
  ClockReplacer clock(2);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kNormal);
  frames[0].pin.store(1);

  EXPECT_FALSE(clock.SelectVictim(PageId{0}, frames).has_value());
  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(LruKReplacer, EvictsNeverRevisitedFirst) {
  std::vector<Frame> frames = ResidentFrames(3);
  LruKReplacer lruk(3);

  lruk.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lruk.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lruk.OnLoad(2, PageId{2}, AccessMode::kNormal);

  EXPECT_EQ(lruk.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 0u);
}

TEST(LruKReplacer, RevisitedFrameOutlivesSingleVisitOnes) {
  std::vector<Frame> frames = ResidentFrames(3);
  LruKReplacer lruk(3);

  lruk.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lruk.OnLoad(1, PageId{1}, AccessMode::kNormal);
  lruk.OnLoad(2, PageId{2}, AccessMode::kNormal);

  lruk.OnHit(0, PageId{0}, AccessMode::kNormal);
  EXPECT_EQ(lruk.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(LruKReplacer, ScanLoadedPageGoesFirst) {
  std::vector<Frame> frames = ResidentFrames(2);
  LruKReplacer lruk(2);

  lruk.OnLoad(0, PageId{0}, AccessMode::kNormal);
  lruk.OnLoad(1, PageId{1}, AccessMode::kScan);

  EXPECT_EQ(lruk.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 1u);
}

TEST(ArcReplacer, StartsByFillingT1) {
  std::vector<Frame> frames = ResidentFrames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);
  arc.OnLoad(2, PageId{2}, AccessMode::kNormal);

  // p = 0，T1 = {2,1,0}，从 T1 尾淘汰最早装入的 0。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 0u);
}

TEST(ArcReplacer, SecondHitPromotesToT2) {
  std::vector<Frame> frames = ResidentFrames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);
  arc.OnHit(0, PageId{0}, AccessMode::kNormal);  // 0 -> T2

  // T1 = {1}，T2 = {0}，p = 0：T1 > p，从 T1 淘汰 1。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 1u);
}

TEST(ArcReplacer, GhostHitOnB1MovesPageToT2) {
  std::vector<Frame> frames = ResidentFrames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{0});                      // 0 -> B1
  arc.OnMiss(PageId{0});                          // 幽灵命中：p -> 1
  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);  // 装回 T2

  // T1 = {}，T2 = {0}，p = 1：T1 <= p，从 T2 淘汰 0。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 0u);
}

// 幽灵命中的步长按"另一条"幽灵链表的长度缩放：B1 命中 p += max(1, |B2|/|B1|)，
// B2 命中 p -= max(1, |B1|/|B2|)（Megiddo–Modha 原文）。这里把 |B1| = 1、
// |B2| = 3 摆好，B1 命中后 p 应当是 3；若把分子分母对调，p 只有 1，接着那次
// 选帧就会从 T1 尾而不是 T2 尾取。
TEST(ArcReplacer, B1GhostHitScalesByTheOtherGhostList) {
  std::vector<Frame> frames = ResidentFrames(4);
  ArcReplacer arc(4, 16);

  // 撑出 B2 = {0, 1, 2}：每次装入后命中一次升到 T2，再淘汰。
  for (size_t page = 0; page < 3; ++page) {
    arc.OnLoad(page, static_cast<PageId>(page), AccessMode::kNormal);
    arc.OnHit(page, static_cast<PageId>(page), AccessMode::kNormal);
  }
  arc.OnEvict(2, PageId{2});
  arc.OnEvict(1, PageId{1});
  arc.OnEvict(0, PageId{0});

  // 再撑出 B1 = {3}。
  arc.OnLoad(0, PageId{3}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{3});

  // B1 命中：p = 0 + max(1, |B2|/|B1|) = 3。调两次也只调一次——同一次缺页
  // 可能因等待而多次经过 OnMiss，重复相加就不是论文的公式了。
  arc.OnMiss(PageId{3});
  arc.OnMiss(PageId{3});
  arc.OnLoad(0, PageId{3}, AccessMode::kNormal);

  // 摆成 |T1| = |T2| = 2。p = 3 时 |T1| 没超过目标，从 T2 尾（2 号帧）淘汰；
  // 若把比例写反、p 只有 1，就会转向 T1 尾（1 号帧）。
  arc.OnLoad(1, PageId{4}, AccessMode::kNormal);
  arc.OnLoad(2, PageId{5}, AccessMode::kNormal);
  arc.OnHit(2, PageId{5}, AccessMode::kNormal);
  arc.OnLoad(3, PageId{6}, AccessMode::kNormal);
  arc.OnHit(3, PageId{6}, AccessMode::kNormal);
  arc.OnLoad(0, PageId{7}, AccessMode::kNormal);

  EXPECT_EQ(arc.SelectVictim(PageId{8}, frames).value_or(kNoFrame), 2u);
}

// OnMiss 对同一条幽灵只调一次：连调两次后 p 必须是 1，写成 2 的话下一次
// 选帧就会从 T2 尾而不是 T1 尾取。
TEST(ArcReplacer, MissOnTheSameGhostEntryAdaptsOnce) {
  std::vector<Frame> frames = ResidentFrames(3);
  ArcReplacer arc(3, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{0});  // B1 = {0}、|B2| = 0 -> δ1 = max(1, 0/1) = 1
  arc.OnMiss(PageId{0});
  arc.OnMiss(PageId{0});
  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);  // 装回 T2

  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);  // T1 = [1]
  arc.OnLoad(2, PageId{2}, AccessMode::kNormal);  // T1 = [2, 1]

  // p = 1 时 |T1| = 2 超过目标，从 T1 尾（1 号帧）淘汰；p = 2 时不超，转向 T2。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 1u);
}

// 扫描装入的页进 T1 头部——按位置是最后被淘汰的，靠冷标记才排到最前。
TEST(ArcReplacer, EvictsScanLoadedPageFirst) {
  std::vector<Frame> frames = ResidentFrames(2);
  ArcReplacer arc(2, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kScan);

  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 1u);
}

TEST(ArcReplacer, NormalHitClearsTheScanColdMark) {
  std::vector<Frame> frames = ResidentFrames(2);
  ArcReplacer arc(2, 8);

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kScan);
  arc.OnHit(1, PageId{1}, AccessMode::kNormal);  // 冷标记清掉，1 提升到 T2

  // T1 = {0}、T2 = {1}，p = 0：1 已是热页，从 T1 尾淘汰 0。
  EXPECT_EQ(arc.SelectVictim(PageId{7}, frames).value_or(kNoFrame), 0u);
}

// 谓词可以反复求值：若 HasEvictable 顺手把热帧降了冷，下面第一次选帧就会
// 直接命中，而不是先把两帧降冷、这一圈选不出来。
TEST(ClockReplacer, HasEvictableIsPure) {
  std::vector<Frame> frames = ResidentFrames(2);
  ClockReplacer clock(2);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kNormal);

  EXPECT_TRUE(clock.HasEvictable(frames));
  EXPECT_TRUE(clock.HasEvictable(frames));
  EXPECT_FALSE(clock.SelectVictim(PageId{0}, frames).has_value());
  EXPECT_EQ(clock.SelectVictim(PageId{0}, frames).value_or(kNoFrame), 0u);
}

TEST(ClockReplacer, HasEvictableSkipsPinnedAndEvictingFrames) {
  std::vector<Frame> frames = ResidentFrames(2);
  ClockReplacer clock(2);

  clock.OnLoad(0, PageId{0}, AccessMode::kNormal);
  clock.OnLoad(1, PageId{1}, AccessMode::kNormal);

  frames[0].pin.store(1);
  frames[1].pin.store(1);
  EXPECT_FALSE(clock.HasEvictable(frames));

  frames[1].pin.store(0);
  EXPECT_TRUE(clock.HasEvictable(frames));

  // 淘汰中的帧正在锁外写回，再选一次会让两个线程写同一帧。
  frames[1].state.store(FrameState::kEvicting, std::memory_order_relaxed);
  EXPECT_FALSE(clock.HasEvictable(frames));
}

// 两次幽灵命中接连发生，中间隔着锁外 I/O：按页记录的判定互不覆盖。
// 单个待定槽会被后者覆盖前者，两页都退回 T1、自适应失效。
TEST(ArcReplacer, GhostHitsSurviveInterleavedMisses) {
  std::vector<Frame> frames = ResidentFrames(2);
  ArcReplacer arc(2, 8);

  // 撑出 B1 = [P1, P0]，两页各占一个帧，便于分别装回。
  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{0});
  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);
  arc.OnEvict(1, PageId{1});

  arc.OnMiss(PageId{0});  // B1 命中，层位记为 T2
  arc.OnMiss(PageId{1});  // 另一次缺页，不得覆盖上一次的判定
  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnLoad(1, PageId{1}, AccessMode::kNormal);

  EXPECT_EQ(arc.t2_size(), 2u);
  EXPECT_EQ(arc.t1_size(), 0u);
}

// 幽灵命中后要装入的页往往正是最老的幽灵，本轮 PushGhost 的容量清理会把
// 它摘掉；OnLoad 再查 ghost_ 就会当成首次装入、退回 T1。层位必须用 OnMiss
// 那一刻的判定。
TEST(ArcReplacer, GhostHitSurvivesCapacityCleanupDuringTheSameMiss) {
  std::vector<Frame> frames = ResidentFrames(2);
  ArcReplacer arc(2, 8);  // 幽灵上限 = 帧数 = 2

  // B2 = [P1, P0]（P0 在最老端），B1 为空。
  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);
  arc.OnHit(0, PageId{0}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{0});
  arc.OnLoad(0, PageId{1}, AccessMode::kNormal);
  arc.OnHit(0, PageId{1}, AccessMode::kNormal);
  arc.OnEvict(0, PageId{1});

  // 命中 B2 里最老的 P0：调整 p，并记下这次是幽灵命中。
  arc.OnMiss(PageId{0});

  // 淘汰一帧时 PushGhost 把幽灵总数推到 3，超过上限，清理较长链（B2）的尾，
  // 正好是 P0。
  arc.OnEvict(0, PageId{7});

  arc.OnLoad(0, PageId{0}, AccessMode::kNormal);

  EXPECT_EQ(arc.t2_size(), 1u);
  EXPECT_EQ(arc.t1_size(), 0u);
}

}  // namespace
}  // namespace mvcc
