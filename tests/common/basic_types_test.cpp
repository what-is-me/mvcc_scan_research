#include <atomic>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "common/basic_types.h"

namespace mvcc {
namespace {

constexpr Timestamp kMaxTimestamp = (uint64_t{1} << 63) - 1;
constexpr PageId kMaxPage = (uint64_t{1} << 48) - 1;

constexpr PageId kPage = 12345678;
constexpr SlotId kSlot = 4095;
constexpr RowId kRowId = RowId::Make(kPage, kSlot);

static_assert(sizeof(VersionStamp) == sizeof(uint64_t));
static_assert(sizeof(RowId) == sizeof(uint64_t));
static_assert(std::is_same_v<UndoRowId, RowId>);
static_assert(std::atomic<VersionStamp>::is_always_lock_free);
static_assert(std::atomic<UndoRowId>::is_always_lock_free);

}  // namespace

TEST(VersionStamp, DefaultIsUnlockedZero) {
  constexpr VersionStamp kInitial;
  EXPECT_EQ(kInitial.raw(), 0u);
  EXPECT_FALSE(kInitial.locked());
  EXPECT_EQ(kInitial.timestamp(), Timestamp{0});
}

TEST(VersionStamp, CommittedKeepsLockBitClear) {
  EXPECT_EQ(VersionStamp::Committed(1).raw(), 1u);
  EXPECT_EQ(VersionStamp::Committed(42).timestamp(), Timestamp{42});
  EXPECT_FALSE(VersionStamp::Committed(42).locked());
  EXPECT_FALSE(VersionStamp::Committed(kMaxTimestamp).locked());
  EXPECT_EQ(VersionStamp::Committed(kMaxTimestamp).timestamp(), kMaxTimestamp);
}

TEST(VersionStamp, LockRoundTrip) {
  constexpr VersionStamp kCommitted = VersionStamp::Committed(42);
  constexpr VersionStamp kLocked = kCommitted.WithLock();

  EXPECT_TRUE(kLocked.locked());
  EXPECT_EQ(kLocked.timestamp(), Timestamp{42});
  EXPECT_EQ(kLocked.WithoutLock(), kCommitted);
  EXPECT_FALSE(kLocked.WithoutLock().locked());
}

TEST(VersionStamp, LockSetsHighestBitOnly) {
  constexpr VersionStamp kCommitted = VersionStamp::Committed(1234);
  EXPECT_EQ(kCommitted.WithLock().raw(), kCommitted.raw() | (uint64_t{1} << 63));
  EXPECT_EQ(kCommitted.WithLock().raw() - (uint64_t{1} << 63), kCommitted.raw());
}

TEST(VersionStamp, LockAndUnlockAreIdempotent) {
  constexpr VersionStamp kCommitted = VersionStamp::Committed(7);
  EXPECT_EQ(kCommitted.WithLock().WithLock(), kCommitted.WithLock());
  EXPECT_EQ(kCommitted.WithoutLock(), kCommitted);
  EXPECT_EQ(kCommitted.WithLock().WithoutLock().WithoutLock(), kCommitted);
}

TEST(VersionStamp, EqualityComparesWholeWord) {
  constexpr VersionStamp kSeven = VersionStamp::Committed(7);
  EXPECT_EQ(kSeven, VersionStamp::Committed(7));
  EXPECT_NE(kSeven, VersionStamp::Committed(8));
  EXPECT_NE(kSeven, kSeven.WithLock());
}

TEST(VersionStamp, FromRawRoundTrip) {
  constexpr uint64_t kRaw = (uint64_t{1} << 63) | 12345;
  constexpr VersionStamp kStamp = VersionStamp::FromRaw(kRaw);

  EXPECT_EQ(kStamp.raw(), kRaw);
  EXPECT_TRUE(kStamp.locked());
  EXPECT_EQ(kStamp.timestamp(), Timestamp{12345});
  EXPECT_EQ(kStamp.WithoutLock(), VersionStamp::Committed(12345));
}

TEST(RowId, DefaultIsPageZeroSlotZero) {
  constexpr RowId kDefault;
  EXPECT_EQ(kDefault.raw(), 0u);
  EXPECT_EQ(kDefault.page(), PageId{0});
  EXPECT_EQ(kDefault.slot(), SlotId{0});
  EXPECT_FALSE(kDefault.is_null());
}

TEST(RowId, PackUnpackRoundTrip) {
  EXPECT_EQ(kRowId.page(), kPage);
  EXPECT_EQ(kRowId.slot(), kSlot);
  EXPECT_EQ(RowId::Make(kRowId.page(), kRowId.slot()), kRowId);
  EXPECT_EQ(RowId::FromRaw(kRowId.raw()), kRowId);
}

TEST(RowId, SlotOccupiesLowSixteenBits) {
  EXPECT_EQ(RowId::Make(0, 1).raw(), 1u);
  EXPECT_EQ(RowId::Make(1, 0).raw(), uint64_t{1} << 16);
  EXPECT_EQ(RowId::Make(1, 1).raw(), (uint64_t{1} << 16) | 1);
}

TEST(RowId, MaxPageAndSlotAreValid) {
  EXPECT_EQ(RowId::Make(kMaxPage, 0).page(), kMaxPage);
  EXPECT_EQ(RowId::Make(0, 0xFFFF).slot(), SlotId{0xFFFF});
  EXPECT_FALSE(RowId::Make(kMaxPage, 0).is_null());
  EXPECT_FALSE(RowId::Make(kMaxPage, 0xFFFE).is_null());
}

TEST(RowId, NullSentinelIsAllOnes) {
  constexpr RowId kNull = RowId::Null();
  EXPECT_EQ(kNull.raw(), ~uint64_t{0});
  EXPECT_TRUE(kNull.is_null());
  EXPECT_EQ(kNull.page(), PageId{0xFFFFFFFFFFFF});
  EXPECT_EQ(kNull.slot(), SlotId{0xFFFF});
}

TEST(RowId, UndoRowIdUsesSameLayout) {
  constexpr UndoRowId kUndo = UndoRowId::Make(7, 9);
  EXPECT_EQ(kUndo.page(), PageId{7});
  EXPECT_EQ(kUndo.slot(), SlotId{9});
  EXPECT_EQ(UndoRowId::Null(), RowId::Null());
}

}  // namespace mvcc
