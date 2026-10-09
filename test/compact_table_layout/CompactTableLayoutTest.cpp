#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "CompactTableLayout.h"
#include "GfxRenderer.h"
#include "TableColumnLayout.h"
#include "TableTextLineOrder.h"

namespace {

size_t countOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t offset = text.find(needle); offset != std::string::npos; offset = text.find(needle, offset + 1)) {
    ++count;
  }
  return count;
}

BlockStyle leftStyle() {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.isRtl = false;
  return style;
}

TEST(CompactTableLayoutTest, SourceFixtureModelsLargeAndUnsupportedTables) {
  std::ifstream fixture(COMPACT_TABLE_FIXTURE_PATH);
  ASSERT_TRUE(fixture.good());
  const std::string source((std::istreambuf_iterator<char>(fixture)), std::istreambuf_iterator<char>());
  const size_t gridStart = source.find("<table class=\"compact-grid\">");
  const size_t gridEnd = source.find("</table>", gridStart);
  ASSERT_NE(gridStart, std::string::npos);
  ASSERT_NE(gridEnd, std::string::npos);
  const std::string grid = source.substr(gridStart, gridEnd - gridStart);
  const size_t captionStart = grid.find("<caption>");
  const size_t firstRowStart = grid.find("<tr>");

  ASSERT_NE(captionStart, std::string::npos);
  ASSERT_NE(firstRowStart, std::string::npos);
  EXPECT_LT(captionStart, firstRowStart);
  EXPECT_NE(grid.find("Table 2-1: Compact table caption"), std::string::npos);
  EXPECT_EQ(countOccurrences(grid, "<tr>"), 33u);  // group heading + header + 31 data rows
  EXPECT_EQ(countOccurrences(grid, "<td"), 31u * 8u);
  EXPECT_EQ(countOccurrences(grid, "<th"), 9u);  // eight headers plus the full-width heading
  EXPECT_NE(source.find("class=\"compact-grid\""), std::string::npos);
  EXPECT_NE(source.find("colspan=\"8\""), std::string::npos);
  EXPECT_NE(source.find("rowspan=\"2\""), std::string::npos);
  EXPECT_NE(source.find("<th"), std::string::npos);
  EXPECT_NE(source.find("text-align: center"), std::string::npos);
  EXPECT_NE(source.find("direction: rtl"), std::string::npos);
  EXPECT_NE(source.find("<strong>"), std::string::npos);
  EXPECT_NE(source.find("nested-table"), std::string::npos);
  EXPECT_NE(source.find("too-wide"), std::string::npos);
  EXPECT_NE(source.find("长"), std::string::npos);
}

TEST(CompactTableLayoutTest, WideLeadingColumnKeepsEightColumnTableLabelsReadable) {
  constexpr uint16_t tableWidth = 480;
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 8, 0, 1), 106);
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 8, 1, 1), 54);
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 8, 0, 8), tableWidth);
  EXPECT_EQ(TableColumnLayout::columnStart(tableWidth, 8, 8), tableWidth);

  // Other grids keep their established equal column sizing.
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 6, 0, 1), 80);
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 6, 5, 1), 80);
}

TEST(CompactTableLayoutTest, VisitsWrappedCellsInVisualReadingOrder) {
  struct Cell {
    std::vector<int> lines;
  };
  struct Row {
    std::vector<Cell> cells;
  };

  const Row row{{{{1, 2}}, {{3}}, {{4, 5}}}};
  std::vector<std::pair<size_t, size_t>> visited;
  ASSERT_TRUE(
      TableTextLineOrder::forEachCellLineInVisualOrder(row, [&](const size_t cellIndex, const size_t lineIndex) {
        visited.emplace_back(cellIndex, lineIndex);
        return true;
      }));

  const std::vector<std::pair<size_t, size_t>> expected{{0, 0}, {1, 0}, {2, 0}, {0, 1}, {2, 1}};
  EXPECT_EQ(visited, expected);
}

TEST(CompactTableLayoutTest, CharacterSpacingCrossesAttachedStyleBoundaries) {
  for (bool rtl : {false, true}) {
    for (int8_t spacing : {-2, 2}) {
      GfxRenderer renderer;
      auto style = leftStyle();
      style.isRtl = rtl;
      CompactTableLayout layout(renderer, 0, 80, 200, 10, 2, style, spacing);
      ASSERT_TRUE(layout.beginRow());
      ASSERT_TRUE(layout.beginCell(false, 1, 0, style));
      ASSERT_TRUE(layout.appendWord("AB", EpdFontFamily::REGULAR, false, false, 0));
      ASSERT_TRUE(layout.appendWord("CD", EpdFontFamily::BOLD, true, false, 0));
      ASSERT_TRUE(layout.endCell({}));
      TableFragmentRow row;
      std::vector<std::shared_ptr<TextBlock>> flattened;
      std::vector<FootnoteEntry> notes;
      uint32_t offset = 0;
      ASSERT_EQ(layout.finishRow(row, flattened, notes, offset), CompactTableLayout::RowResult::Ok);
      ASSERT_EQ(row.cells.size(), 1u);
      ASSERT_EQ(row.cells[0].lines.size(), 1u);
      const auto& positions = row.cells[0].lines[0]->xPositions;
      ASSERT_EQ(positions.size(), 2u);
      // "AB"/"CD" each measure as 2 (one glyph-pixel each) plus `spacing` for their one
      // internal glyph gap (GfxRenderer stub's getTextAdvanceX), and gapBefore() adds the
      // stub's getKerning(spacing) - getKerning(0) = ((spacing+1)>>1) at the attached-style
      // boundary between them. LTR accumulates from x=0 and clamps to 0 (an EPD coordinate
      // can't go negative), which only bites when spacing is negative enough to swing the
      // running total below zero; RTL accumulates from the right edge, which this cell's
      // width keeps comfortably positive, so it never hits that floor.
      const int wordWidth = 2 + spacing;
      const int gap = (spacing + 1) >> 1;
      const int expected = rtl ? wordWidth + gap : std::max(0, wordWidth + gap);
      EXPECT_EQ(rtl ? positions[0] - positions[1] : positions[1] - positions[0], expected);
    }
  }
}

TEST(CompactTableLayoutTest, CompactLayoutUsesWideLeadingColumnForEightCellRows) {
  GfxRenderer renderer;
  renderer.codepointWidth = 10;
  CompactTableLayout layout(renderer, 0, 480, 300, 10, 6, leftStyle());
  ASSERT_TRUE(layout.beginRow());

  for (uint8_t column = 0; column < 8; ++column) {
    ASSERT_TRUE(layout.beginCell(column == 0, 1, 0, leftStyle()));
    ASSERT_TRUE(layout.appendWord(column == 0 ? "Oatmeal" : "100", EpdFontFamily::REGULAR, false, false, 0));
    ASSERT_TRUE(layout.endCell({}));
  }

  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  ASSERT_EQ(layout.finishRow(row, flatLines, footnotes, offset), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 8u);
  EXPECT_EQ(row.cells.front().lines.size(), 1u);
}

TEST(CompactTableLayoutTest, PreservesColspanAndFullWidthRows) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 80, 200, 10, 2, leftStyle());
  ASSERT_TRUE(layout.valid());

  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(true, 8, 12, leftStyle()));
  ASSERT_TRUE(layout.appendWord("Group", EpdFontFamily::BOLD, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  const auto result = layout.finishRow(row, flatLines, footnotes, offset);

  EXPECT_EQ(result, CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 1u);
  EXPECT_EQ(row.cells.front().colSpan, 8);
  EXPECT_EQ(layout.fragmentColumnCount(), 1);
}

TEST(CompactTableLayoutTest, BreaksUtf8AndOversizedCodepointsWithoutAbort) {
  GfxRenderer renderer;
  renderer.codepointWidth = 3;
  CompactTableLayout layout(renderer, 0, 12, 200, 10, 1, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 0, leftStyle()));
  const std::string longUtf8 = "长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长";
  ASSERT_TRUE(layout.appendWord(longUtf8, EpdFontFamily::REGULAR, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  EXPECT_EQ(layout.finishRow(row, flatLines, footnotes, offset), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 1u);
  EXPECT_GE(row.cells.front().lines.size(), 2u);

  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 0, leftStyle()));
  renderer.codepointWidth = 32;
  ASSERT_TRUE(layout.appendWord("☃", EpdFontFamily::REGULAR, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  row = {};
  flatLines.clear();
  footnotes.clear();
  const auto oversizedResult = layout.finishRow(row, flatLines, footnotes, offset);
  EXPECT_NE(oversizedResult, CompactTableLayout::RowResult::Abort);
  ASSERT_EQ(row.cells.size(), 1u);
  ASSERT_EQ(row.cells.front().lines.size(), 1u);
  EXPECT_EQ(row.cells.front().lines.front()->words.size(), 1u);
}

// A single-column cell containing two five-letter words separated by a space.
// At 0 tracking both words fit on one line; widened per-glyph tracking must be
// reflected in measure()'s width so the same content now wraps to a second
// line -- proving layout (not just draw) accounts for character spacing.
// characterSpacing is a whole-table constant fixed at construction (mirroring
// ChapterHtmlSlimParser's own self->characterSpacing at table-start), not a
// per-cell BlockStyle field -- CompactTableLayout never reads the latter.
int lineCountForCharacterSpacing(const int8_t spacing) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 20, 200, 10, 0, leftStyle(), spacing);
  const BlockStyle cellStyle = leftStyle();
  if (!layout.beginRow()) return -1;
  if (!layout.beginCell(false, 1, 0, cellStyle)) return -1;
  if (!layout.appendWord("ABCDE", EpdFontFamily::REGULAR, false, false, 0)) return -1;
  if (!layout.appendWord("FGHIJ", EpdFontFamily::REGULAR, false, false, 0)) return -1;
  if (!layout.endCell({})) return -1;

  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  if (layout.finishRow(row, flatLines, footnotes, offset) != CompactTableLayout::RowResult::Ok) return -1;
  if (row.cells.size() != 1) return -1;
  return static_cast<int>(row.cells.front().lines.size());
}

TEST(CompactTableLayoutTest, CharacterSpacingWideningIsReflectedInCellWrapping) {
  EXPECT_EQ(lineCountForCharacterSpacing(0), 1);
  EXPECT_EQ(lineCountForCharacterSpacing(2), 2);
}

TEST(CompactTableLayoutTest, UnsupportedRowsFlattenDeterministically) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 80, 200, 10, 2, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 9, 0, leftStyle()));
  ASSERT_TRUE(layout.appendWord("too-wide", EpdFontFamily::REGULAR, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  layout.markUnsupported();
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  EXPECT_EQ(layout.finishRow(row, flatLines, footnotes, offset), CompactTableLayout::RowResult::Flatten);
  EXPECT_FALSE(flatLines.empty());
}

}  // namespace
