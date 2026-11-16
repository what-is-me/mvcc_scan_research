#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "common/aligned_buffer.h"

namespace mvcc {
namespace {

TEST(AlignedBuffer, AlignsWholeBufferToSectorBoundary) {
  AlignedBuffer buffer(3 * kSectorBytes);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(buffer.data()) % kSectorBytes, 0u);
  EXPECT_EQ(buffer.size(), 3 * kSectorBytes);
  EXPECT_EQ(buffer.span().size(), 3 * kSectorBytes);
}

TEST(AlignedBuffer, EveryPageSlotStaysAligned) {
  AlignedBuffer buffer(8 * kSectorBytes);
  for (size_t slot = 0; slot < 8; ++slot) {
    const std::span<std::byte> page = buffer.span().subspan(slot * kSectorBytes, kSectorBytes);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(page.data()) % kSectorBytes, 0u) << slot;
  }
}

TEST(AlignedBuffer, AcceptsCustomAlignment) {
  AlignedBuffer buffer(1024, 64);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(buffer.data()) % 64, 0u);
  EXPECT_EQ(buffer.size(), 1024u);
}

}  // namespace
}  // namespace mvcc
