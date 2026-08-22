#pragma once

#include <cstdint>

#include "components/themes/lyra/LyraTheme.h"

namespace MinimalMetrics {
constexpr int coverWidthForHeight(const int coverHeight) {
  return static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
}

constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = LyraMetrics::values;
  // Was 50. The header only needs room for a battery icon and percentage
  // text; other themes run their own header as short as 28 (LyraCarousel)
  // or 40 (Base). 36 stays comfortably inside that range while handing the
  // difference to the companion strip below -- the button-hint row it has
  // to clear on non-touch devices (X3/X4) doesn't shrink with touch the way
  // Dashboard's footer does, so every pixel gained above it matters more
  // there than it does on X4 Pro.
  v.homeTopPadding = 36;
  // Was 583, taller than the cover art itself ever renders (capped at
  // homeCoverImageHeight, 525 below) -- the difference was pure letterboxing
  // above and below the cover, plus that much less room for the companion
  // strip beneath it. Trimmed further to 480 (proportionally narrower too,
  // via coverWidthForHeight) for the same reason: X3/X4 have no touch-based
  // slack anywhere else to give the companion, so the cover is the only
  // remaining lever for them specifically.
  v.homeCoverHeight = 480;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  v.tabBarAppearance = ThemeTabBarAppearance::BorderedText;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverWidth = coverWidthForHeight(values.homeCoverHeight);
constexpr int homeCoverImageWidth = homeCoverWidth;
constexpr int homeCoverImageHeight = 525;
}  // namespace MinimalMetrics

struct GlobalReadingStats;

class MinimalTheme : public LyraTheme {
 public:
  static void setHomeButtonHintSelection(int selectedIndex);
  static Rect buttonMenuPanelRect(const GfxRenderer& renderer, int buttonCount);
  static int compactFileBrowserRowHeightFor(const GfxRenderer& renderer);
  static void drawCompactFileBrowserList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                         const std::function<std::string(int index)>& rowTitle,
                                         const std::function<std::string(int index)>& rowSubtitle,
                                         const std::function<UIIcon(int index)>& rowIcon,
                                         const std::function<std::string(int index)>& rowValue,
                                         const std::function<bool(int index)>& rowDimmed = nullptr,
                                         bool showSelection = true, bool uniformRowHeight = false,
                                         bool allowTwoLineTitles = true);

  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool readerContext = false) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                         int& index) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<bool(int index)>& isHeader = nullptr, int rowHeightScale = 1,
                bool showSelection = true) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                       bool allowInvertedText = false) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  // The cover tile reserves more height (homeCoverTileHeight) than the drawn
  // cover actually uses (homeCoverHeight), but the gap below it isn't empty
  // when there's reading progress to show: drawProgressBlock puts the
  // duration/bar/percent there. Placement starts below that block, not below
  // the cover, so the companion never lands on top of it.
  HomeCompanionLayout getHomeCompanionLayout(const GfxRenderer& renderer, Rect menuRect, Rect coverRect,
                                             int buttonCount, const std::function<std::string(int index)>& buttonLabel,
                                             int hintsTop, const HomeCompanionContext& context) const override;
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats = nullptr,
                       float progressPercent = -1.0f, bool inverted = false) const;
  void drawStatsSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                            const GlobalReadingStats* globalStats, float progressPercent = -1.0f,
                            bool inverted = false) const;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<const char*(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  bool usesCompactFileBrowserRows() const override { return true; }
  int compactFileBrowserRowHeight(const GfxRenderer& renderer) const override;
};
