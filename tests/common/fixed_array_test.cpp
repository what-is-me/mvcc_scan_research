#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/fixed_array.h"

namespace mvcc {

TEST(FixedArray, ReportsItsSize) {
  const FixedArray<int> values(64);
  EXPECT_EQ(values.size(), 64u);
  EXPECT_FALSE(values.empty());

  const FixedArray<int> none(0);
  EXPECT_TRUE(none.empty());
}

TEST(FixedArray, ProvidesElementAccess) {
  FixedArray<int> values(3);
  values[0] = 10;
  values[1] = 20;
  values[2] = 30;

  EXPECT_EQ(values[0], 10);
  EXPECT_EQ(values[1], 20);
  EXPECT_EQ(values[2], 30);
}

TEST(FixedArray, SpanCoversTheWholeStorage) {
  FixedArray<int> values(8);
  std::span<int> view = values.span();

  EXPECT_EQ(view.size(), 8u);
  EXPECT_EQ(view.data(), &values[0]);
  EXPECT_EQ(&view.back(), &values[7]);
}

TEST(FixedArray, HoldsNonMovableElements) {
  FixedArray<std::mutex> locks(16);
  const std::lock_guard<std::mutex> guard(locks[3]);

  EXPECT_EQ(locks.size(), 16u);
  EXPECT_EQ(&locks[0], locks.span().data());
}

TEST(FixedArray, GivesEachInstanceItsOwnStorage) {
  FixedArray<int> first(4);
  FixedArray<int> second(4);

  EXPECT_NE(first.span().data(), second.span().data());

  first[0] = 1;
  second[0] = 2;
  EXPECT_EQ(first[0], 1);
  EXPECT_EQ(second[0], 2);
}

TEST(FixedArray, AllocatesRawBytesWithoutZeroing) {
  FixedArray<std::byte> bytes(1024);
  bytes[0] = std::byte{0x7F};
  bytes[1023] = std::byte{0x01};

  EXPECT_EQ(bytes.size(), 1024u);
  EXPECT_EQ(std::to_integer<int>(bytes[0]), 0x7F);
  EXPECT_EQ(std::to_integer<int>(bytes[1023]), 0x01);
}

}  // namespace mvcc
