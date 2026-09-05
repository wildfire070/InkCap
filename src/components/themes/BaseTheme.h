#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;
struct BookReadingStats;
struct GlobalReadingStats;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

enum class ThemeTabBarAppearance : uint8_t {
  Pill,
  BorderedText,
};

// The Classic and Lyra-derived themes share the same status-bar artwork.
// Keep its bounds in one place so changing a theme cannot subtly shift the
// clock or battery percentage relative to the other main themes.
namespace StatusBarMetrics {
constexpr int batteryWidth = 15;
constexpr int batteryHeight = 12;
}  // namespace StatusBarMetrics

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  // FreeInkUI screens take shape from the theme and size from UIScale.
  int listRowGap;
  int listRowRadius;
  int listInset;
  int listSidePadding;
  int listSelectionStyle;
  int listScrollWidth;
  int listScrollSide;
  bool listTitleBold;
  int headerSidePadding;
  int headerUnderlineSize;
  int headerTitleAlign;
  int headerBatterySide;
  bool headerBatteryDetached;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;
  ThemeTabBarAppearance tabBarAppearance;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionHPadding;
  int optionPopupSelectionVPadding;
  int optionPopupTitleGap;
  bool optionPopupUseSmallFont;
  bool optionPopupOptionFontBold;
  int optionPopupSelectionRadius;
  bool optionPopupSelectionLight;
  bool optionPopupDrawAllRows;
  int optionPopupDialogSideMargin;
  bool optionPopupTitleSeparator;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;
};

enum UIIcon {
  Folder,
  Text,
  Image,
  Book,
  BookmarkIcon,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  Chart,
  BookFusion,
  Ao3,
  Star,
  Check,
  Arrow,
  Files
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = StatusBarMetrics::batteryWidth,
                                 .batteryHeight = StatusBarMetrics::batteryHeight,
                                 .topPadding = 5,
                                 .batteryBarHeight = 45,
                                 .headerHeight = 70,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 .listInset = 0,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 18,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 1,
                                 .headerBatterySide = 0,
                                 .headerBatteryDetached = true,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .tabBarAppearance = ThemeTabBarAppearance::Pill,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 370,
                                 .homeCoverTileHeight = 370,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 56,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionHPadding = 8,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupTitleGap = 10,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = true,
                                 .optionPopupSelectionRadius = 0,
                                 .optionPopupSelectionLight = false,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

// Where the home companion draws, plus any width a theme gives up to make room.
// A zero width means "draw that element as usual".
struct HomeCompanionLayout {
  Rect region;         // empty when the theme has no room to spare
  int menuWidth = 0;   // narrows the menu so the companion can sit beside it
  int coverWidth = 0;  // narrows the cover tile, which re-centres the cover leftwards
};

// What's actually about to be drawn below the cover on this render, for themes
// whose companion placement depends on it: Dashboard measures the real
// title/chapter wrap instead of assuming its worst case, and Minimal starts
// below the progress block (duration/bar/percent) rather than assuming the
// strip under the cover is always empty. Themes that don't need this ignore
// it. `bookTitle`/`chapterTitle` are already resolved the same way the actual
// draw call resolves them (title-or-path, chapter-or-author) so a theme
// measuring them doesn't need to re-derive the fallback rule.
struct HomeCompanionContext {
  const char* bookTitle = nullptr;
  const char* chapterTitle = nullptr;
  const BookReadingStats* stats = nullptr;
  float progressPercent = -1.0f;
};

// Below this the strip under the menu cannot hold a character and its status,
// so a theme with somewhere better to put the companion should offer it.
inline constexpr int kMinCompanionStripHeight = 60;

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage = true,
                       bool foregroundBlack = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage = true,
                        bool foregroundBlack = true) const;  // Right aligned (UI headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage,
                               bool foregroundBlack = true) const;
  // Button hint labels use three states: non-empty labels draw an active hint,
  // an empty string clears an inactive slot, and nullptr preserves the existing
  // background without drawing or registering a target.
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4, bool allowInvertedText = false) const;
  // Where drawButtonHints (non-inverted) puts button `index` (0=btn1..3=btn4)
  // on screen -- the same box drawButtonHints itself fills/outlines, exposed
  // so other UI (e.g. a caption that needs to sit exactly over one of these
  // hints) can ask the active theme instead of guessing its geometry.
  virtual Rect buttonHintRect(const GfxRenderer& renderer, int index) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  virtual int getMenuRowHeight(const GfxRenderer& renderer) const;
  // Height the drawn menu actually occupies inside `rect`, gaps included.
  // Anything positioned below the menu must ask for this rather than deriving
  // it from the metrics table: each theme spaces its rows differently, and
  // RoundedRaff both sizes rows from the font and paginates to fit `rect`.
  virtual int getMenuContentHeight(const GfxRenderer& renderer, Rect rect, int buttonCount) const;
  // Where the home screen's reading companion draws, and what gives up room for
  // it. The strip between the last menu row and the button hints is only big
  // enough on some theme and menu-length combinations; a theme that can spare a
  // column beside its menu or its cover tile says so by narrowing one of them.
  // `hintsTop` is the first row the button hints occupy; `context` carries what
  // else is about to be drawn below the cover, for themes whose placement
  // depends on it.
  virtual HomeCompanionLayout getHomeCompanionLayout(const GfxRenderer& renderer, Rect menuRect, Rect coverRect,
                                                     int buttonCount,
                                                     const std::function<std::string(int index)>& buttonLabel,
                                                     int hintsTop, const HomeCompanionContext& context) const;
  virtual int getListRowStep(bool hasSubtitle, int rowHeightScale = 1) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle, int rowHeightScale = 1) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr,
                        const std::function<bool(int index)>& isHeader = nullptr, int rowHeightScale = 1,
                        bool showSelection = true) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                          bool readerContext = false) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                                 int& index) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                   const BookReadingStats* stats = nullptr, float progressPercent = -1.0f,
                                   const GlobalReadingStats* globalStats = nullptr,
                                   const char* currentChapterTitle = nullptr) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<const char*(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                               int selectedIndex, bool showConfirmationFooter = false,
                               const char* cancelLabel = nullptr, const char* saveLabel = nullptr,
                               bool saveFocused = false, int primaryOptionIndex = -1, const char* noteLabel = nullptr,
                               const char* noteBody = nullptr, const std::vector<bool>& disabledOptions = {},
                               int firstOptionIndex = -1) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  // title is borrowed and must stay alive for the call; pass nullptr or "" for none.
  virtual void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                             const int pageCount, const char* title, const int paddingBottom = 0,
                             const int textYOffset = 0, const bool isPageBookmarked = false,
                             const char* timeLeftLabel = nullptr, bool darkMode = false,
                             float chapterProgressPercent = -1.0f, int stableCurrentPage = 0, int stablePageCount = 0,
                             bool showProgress = true, bool pageCountEstimated = false) const;
  virtual void drawTopStatusBarClock(const GfxRenderer& renderer, int topY = -1, const char* previewTime = nullptr,
                                     bool readerContext = true, int textYOffset = 0, bool darkMode = false,
                                     bool forceVisible = false) const;
  virtual void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }
  virtual bool usesCompactFileBrowserRows() const { return false; }
  virtual int compactFileBrowserRowHeight(const GfxRenderer&) const { return BaseMetrics::values.listRowHeight; }
  virtual void drawCarouselBorder(GfxRenderer& renderer, Rect coverRect, const std::vector<RecentBook>& recentBooks,
                                  int centerIdx, bool inCarouselRow) const {}

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static constexpr int homeHeaderTopInset = 2;
  static int homeHeaderClockTextYOffset(const GfxRenderer& renderer);
  static Rect buttonMenuTouchTarget(Rect rowRect, Rect menuRect, bool isLastItem, int rowSpacing);
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight,
                                 bool foregroundBlack = true);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY, bool foregroundBlack = false);
};
