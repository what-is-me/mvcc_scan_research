#include <cstddef>

#include <gtest/gtest.h>

#include "buffer/page_table.h"

namespace mvcc {

TEST(DirectPageTable, StartsEmptyAndRoundTrips) {
  DirectPageTable table(8);
  EXPECT_FALSE(table.Find(0).has_value());

  table.Insert(0, 3);
  ASSERT_TRUE(table.Find(0).has_value());
  EXPECT_EQ(*table.Find(0), 3u);

  table.Erase(0);
  EXPECT_FALSE(table.Find(0).has_value());
}

TEST(DirectPageTable, KeepsPagesIndependent) {
  DirectPageTable table(8);
  for (size_t page = 0; page < 8; ++page) table.Insert(page, page + 100);

  for (size_t page = 0; page < 8; ++page) EXPECT_EQ(*table.Find(page), page + 100);

  table.Erase(3);
  EXPECT_FALSE(table.Find(3).has_value());
  EXPECT_EQ(*table.Find(4), 104u);
}

TEST(FrameHashTable, FindsEveryInsertedPage) {
  FrameHashTable table(16);
  for (size_t frame = 0; frame < 16; ++frame) table.Insert(frame * 7, frame);

  for (size_t frame = 0; frame < 16; ++frame) {
    const auto found = table.Find(frame * 7);
    ASSERT_TRUE(found.has_value()) << "page " << frame * 7;
    EXPECT_EQ(*found, frame);
  }
  EXPECT_FALSE(table.Find(1'000'000).has_value());
}

TEST(FrameHashTable, ErasesFromMiddleOfChain) {
  FrameHashTable table(16);
  for (size_t frame = 0; frame < 16; ++frame) table.Insert(frame, frame);

  table.Erase(5);

  EXPECT_FALSE(table.Find(5).has_value());
  for (size_t frame = 0; frame < 16; ++frame) {
    if (frame == 5) continue;
    EXPECT_TRUE(table.Find(frame).has_value()) << "page " << frame;
  }
}

TEST(FrameHashTable, ReusesFrameAfterErase) {
  FrameHashTable table(4);
  table.Insert(0, 0);
  table.Insert(1, 1);

  table.Erase(0);
  table.Insert(2, 0);

  EXPECT_EQ(*table.Find(2), 0u);
  EXPECT_FALSE(table.Find(0).has_value());
  EXPECT_EQ(*table.Find(1), 1u);
}

TEST(FrameHashTable, IgnoresEraseOfAbsentPage) {
  FrameHashTable table(4);
  table.Insert(3, 0);

  table.Erase(4);

  EXPECT_EQ(*table.Find(3), 0u);
}

}  // namespace mvcc
