#include <cstddef>

#include <gtest/gtest.h>

#include "storage/table_layout.h"

namespace mvcc {

TEST(TableLayout, DerivesGeometryFromDefaultParameters) {
  const TableLayout layout(8192, 128, 2'000'000);

  EXPECT_EQ(layout.page_size(), 8192u);
  EXPECT_EQ(layout.row_size(), 128u);
  EXPECT_EQ(layout.data_size(), 112u);
  EXPECT_EQ(layout.rows_per_page(), 64u);
  EXPECT_EQ(layout.row_count(), 2'000'000u);
  EXPECT_EQ(layout.page_count(), 31250u);
  EXPECT_EQ(layout.file_size(), 31250u * 8192u);
}

TEST(TableLayout, RoundsRowCountUpToPageBoundary) {
  const TableLayout layout(8192, 128, 500'000);

  EXPECT_EQ(layout.rows_per_page(), 64u);
  EXPECT_EQ(layout.row_count(), 500'032u);
  EXPECT_EQ(layout.page_count(), 7813u);
  EXPECT_EQ(layout.row_count(), layout.page_count() * layout.rows_per_page());
}

TEST(TableLayout, LeavesRowCountUntouchedWhenAlreadyAligned) {
  const TableLayout layout(4096, 64, 64'000);

  EXPECT_EQ(layout.rows_per_page(), 64u);
  EXPECT_EQ(layout.row_count(), 64'000u);
  EXPECT_EQ(layout.page_count(), 1000u);
}

TEST(TableLayout, KeepsRowsPerPageConstantAcrossSupportedSizes) {
  for (const size_t page_size : {4096u, 8192u, 16384u}) {
    for (const size_t row_size : {64u, 128u, 256u}) {
      const TableLayout layout(page_size, row_size, 1000);
      EXPECT_GE(layout.rows_per_page(), 1u);
      EXPECT_LE(layout.rows_per_page() * layout.row_size(), page_size);
      EXPECT_LT(page_size - layout.rows_per_page() * layout.row_size(), row_size);
    }
  }
}

TEST(TableLayout, MapsLinearRowToRowId) {
  const TableLayout layout(8192, 128, 1'000'000);

  EXPECT_EQ(layout.RowIdAt(0), RowId::Make(0, 0));
  EXPECT_EQ(layout.RowIdAt(63), RowId::Make(0, 63));
  EXPECT_EQ(layout.RowIdAt(64), RowId::Make(1, 0));
  EXPECT_EQ(layout.RowIdAt(1'000'000), RowId::Make(15'625, 0));
}

TEST(TableLayout, MapsRowIdBackToLinearRow) {
  const TableLayout layout(8192, 128, 1'000'000);

  EXPECT_EQ(layout.LinearRow(RowId::Make(0, 0)), 0u);
  EXPECT_EQ(layout.LinearRow(RowId::Make(3, 7)), 3u * 64u + 7u);
  for (const size_t linear : {0u, 1u, 63u, 64u, 999'999u}) {
    EXPECT_EQ(layout.LinearRow(layout.RowIdAt(linear)), linear);
  }
}

TEST(TableLayout, RowIdRoundTripCoversLastRowOfLastPage) {
  const TableLayout layout(8192, 128, 500'000);
  const size_t last_linear = layout.row_count() - 1;

  EXPECT_EQ(layout.LinearRow(layout.RowIdAt(last_linear)), last_linear);
  EXPECT_EQ(layout.RowIdAt(last_linear).page(), PageId{layout.page_count() - 1});
}

}  // namespace mvcc
