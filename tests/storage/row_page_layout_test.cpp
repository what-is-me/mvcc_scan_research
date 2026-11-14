#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "storage/page.h"
#include "storage/row.h"
#include "storage/table_layout.h"

namespace mvcc {
namespace {

// 与真实帧一致：页缓冲区按 64 字节对齐，足够放下最大页。
struct AlignedPageBuffer {
  alignas(64) std::array<std::byte, 16384> bytes{};
};

std::span<std::byte> RowSpan(AlignedPageBuffer& buffer, size_t row_size) {
  return {buffer.bytes.data(), row_size};
}

std::span<std::byte> PageSpan(AlignedPageBuffer& buffer, const TableLayout& layout,
                              size_t page_index = 0) {
  return {buffer.bytes.data() + page_index * layout.page_size(), layout.page_size()};
}

}  // namespace

TEST(RowLayout, HeaderIsSixteenBytes) {
  EXPECT_EQ(sizeof(RowHeader), 16u);
  EXPECT_EQ(offsetof(RowHeader, version), 0u);
  EXPECT_EQ(offsetof(RowHeader, undo_row_id), 8u);
}

TEST(RowLayout, WritesAndReadsHeaderFields) {
  AlignedPageBuffer buffer;
  Row row(RowSpan(buffer, 128));

  row.set_version(VersionStamp::Committed(7).WithLock());
  row.set_undo_row_id(RowId::Make(3, 9));

  EXPECT_TRUE(row.version().locked());
  EXPECT_EQ(row.version().timestamp(), Timestamp{7});
  EXPECT_EQ(row.undo_row_id(), RowId::Make(3, 9));
  EXPECT_EQ(row.header().undo_row_id.slot(), SlotId{9});
}

TEST(RowLayout, DataAreaStartsRightAfterHeader) {
  AlignedPageBuffer buffer;
  Row row(RowSpan(buffer, 128));

  EXPECT_EQ(row.data().size(), 112u);
  EXPECT_EQ(row.data().data(), buffer.bytes.data() + 16);
}

TEST(RowLayout, DataAreaIsOpaqueAndFixedLength) {
  AlignedPageBuffer buffer;
  Row row(RowSpan(buffer, 64));

  std::fill(row.data().begin(), row.data().end(), std::byte{0xAB});

  EXPECT_EQ(std::to_integer<int>(row.data().front()), 0xAB);
  EXPECT_EQ(std::to_integer<int>(row.data().back()), 0xAB);
  EXPECT_EQ(std::to_integer<int>(buffer.bytes[64 - 1]), 0xAB);
  EXPECT_EQ(std::to_integer<int>(buffer.bytes[64]), 0x00);
  EXPECT_EQ(row.version(), VersionStamp{});
  EXPECT_EQ(row.undo_row_id(), RowId{});
}

TEST(PageLayout, HoldsRowsPerPageWithFixedStride) {
  const TableLayout layout(4096, 128, 1000);
  AlignedPageBuffer buffer;
  Page page(PageSpan(buffer, layout), layout);

  EXPECT_EQ(page.row_count(), 32u);
  EXPECT_EQ(page.bytes().size(), layout.page_size());

  for (size_t slot = 0; slot < page.row_count(); ++slot) {
    page.RowAt(slot).set_version(VersionStamp::Committed(slot + 1));
    page.RowAt(slot).set_undo_row_id(RowId::Make(0, static_cast<SlotId>(slot)));
  }

  for (size_t slot = 0; slot < page.row_count(); ++slot) {
    EXPECT_EQ(page.RowAt(slot).version(), VersionStamp::Committed(slot + 1));
    EXPECT_EQ(page.RowAt(slot).undo_row_id(), RowId::Make(0, static_cast<SlotId>(slot)));
  }
}

TEST(PageLayout, RowsSitAtSlotTimesRowSize) {
  const TableLayout layout(4096, 128, 1000);
  AlignedPageBuffer buffer;
  Page page(PageSpan(buffer, layout), layout);

  for (size_t slot = 0; slot < 5; ++slot) {
    EXPECT_EQ(reinterpret_cast<std::byte*>(&page.RowAt(slot).header()),
              buffer.bytes.data() + slot * layout.row_size());
  }
}

TEST(PageLayout, KeepsEveryRowHeaderEightByteAligned) {
  const TableLayout layout(8192, 96, 1000);
  AlignedPageBuffer buffer;
  Page page(PageSpan(buffer, layout), layout);

  for (size_t slot = 0; slot < page.row_count(); ++slot) {
    const auto address = reinterpret_cast<std::uintptr_t>(&page.RowAt(slot).header());
    EXPECT_EQ(address % 8, 0u);
  }
}

TEST(PageLayout, LeavesTailWasteWhenRowSizeDoesNotDividePage) {
  const TableLayout layout(8192, 96, 1000);
  EXPECT_EQ(layout.rows_per_page(), 85u);

  AlignedPageBuffer buffer;
  Page page(PageSpan(buffer, layout), layout);

  const size_t used = page.row_count() * layout.row_size();
  EXPECT_EQ(used, 8160u);
  EXPECT_EQ(layout.page_size() - used, 32u);
  EXPECT_EQ(std::to_integer<int>(buffer.bytes[used]), 0x00);
  EXPECT_EQ(std::to_integer<int>(buffer.bytes[layout.page_size() - 1]), 0x00);
}

TEST(PageLayout, SeparatesRowsFromAdjacentPages) {
  const TableLayout layout(4096, 128, 1000);
  AlignedPageBuffer buffer;
  Page first(PageSpan(buffer, layout, 0), layout);
  Page second(PageSpan(buffer, layout, 1), layout);

  first.RowAt(0).set_undo_row_id(RowId::Make(1, 1));
  second.RowAt(0).set_undo_row_id(RowId::Make(2, 2));

  EXPECT_EQ(first.RowAt(0).undo_row_id(), RowId::Make(1, 1));
  EXPECT_EQ(second.RowAt(0).undo_row_id(), RowId::Make(2, 2));
  EXPECT_EQ(first.RowAt(layout.rows_per_page() - 1).undo_row_id(), RowId{});
}

}  // namespace mvcc
