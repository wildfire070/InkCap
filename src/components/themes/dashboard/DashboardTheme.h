#pragma once

#include <cstdint>

#include "components/themes/minimal/MinimalTheme.h"

namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 445;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverImageWidth = 296;
constexpr int homeCoverImageHeight = 444;
}  // namespace DashboardMetrics

class DashboardTheme : public MinimalTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                       const GlobalReadingStats* globalStats, float progressPercent = -1.0f,
                       const char* currentChapterTitle = nullptr, bool inverted = false) const;
  // Unlike Minimal, the space below Dashboard's cover isn't empty: it holds the
  // book title/chapter text (height varies with wrap) above a fixed footer
  // stats row. Placement has to clear both, measuring the actual title/chapter
  // wrap for this book (via context) rather than assuming the longest one --
  // the worst case reserves more than the footer leaves free on real hardware,
  // which hid the companion on every book.
  HomeCompanionLayout getHomeCompanionLayout(const GfxRenderer& renderer, Rect menuRect, Rect coverRect,
                                             int buttonCount, const std::function<std::string(int index)>& buttonLabel,
                                             int hintsTop, const HomeCompanionContext& context) const override;
};
