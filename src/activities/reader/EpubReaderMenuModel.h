#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class EpubReaderMenuAction : uint8_t {
  SELECT_CHAPTER,
  FOOTNOTES,
  GO_TO_PERCENT,
  GO_TO_STABLE_PAGE,
  AUTO_PAGE_TURN,
  ROTATE_SCREEN,
  SCREENSHOT,
  DISPLAY_QR,
  GO_HOME,
  SYNC,
  BOOKFUSION_SYNC,
  NEARBY_POSITION_SYNC,
  SEND_NEARBY_BOOK,
  DELETE_STATS,
  DELETE_CACHE,
  RESET_READING_PACE,
  READING_STATS,
  TOGGLE_BOOK_STATS_TRACKING,
  TOGGLE_COMPLETED,
  CYCLE_STATUS,  // AO3 fics only: cycle the 5-state AO3 reading status
  // Standalone Archive File / Restore, independent of TOGGLE_COMPLETED -- reads as whichever of the
  // two applies to the book's current location. AO3 fics use their own Archive Fic / Restore in the
  // AO3 book menu instead (which also removes the fic from the AO3 index); this is for plain epubs.
  TOGGLE_ARCHIVED,
  READER_OPTIONS,
  CONTROLS_OPTIONS,
  BOOKMARK_TOGGLE,
  VIEW_BOOKMARKS,
  DELETE_BOOKMARKS,
  SAVE_CLIPPING,
  VIEW_CLIPPINGS,
  LOOKUP,
  LOOKUP_HISTORY,
  SET_BOOK_DICTIONARY,
  STATUS_BAR_SETTINGS,
  RESET_BOOK_READER_SETTINGS,
};

enum class ReaderDrawerTab : uint8_t { Font = 0, Layout = 1, More = 2, Location = 3, Settings = 4, Count };

constexpr size_t READER_DRAWER_TAB_COUNT = static_cast<size_t>(ReaderDrawerTab::Count);
constexpr ReaderDrawerTab adjacentReaderDrawerTab(const ReaderDrawerTab tab, const bool forward) {
  const size_t current = static_cast<size_t>(tab);
  return static_cast<ReaderDrawerTab>((current + (forward ? 1 : READER_DRAWER_TAB_COUNT - 1)) %
                                      READER_DRAWER_TAB_COUNT);
}
constexpr uint16_t READER_AUTO_PAGE_TURN_MIN_SECONDS = 5;
constexpr uint16_t READER_AUTO_PAGE_TURN_MAX_SECONDS = 120;

enum class ReaderDrawerPane : uint8_t {
  Root = 0,
  ReaderFont,
  FontFamily,
  Spacing,
  Margins,
  Chapters,
  Percent,
  StablePage,
  AutoPageTurn,
  Dictionary,
  DictionaryFont,
  EnumOptions,
  TtfRendering,
  CharacterSpacing,
};

enum class ReaderButtonSliderInput : uint8_t { Next, Previous, Confirm, Back };
enum class ReaderButtonSliderAction : uint8_t { None, Increase, Decrease, LeavePane };

struct ReaderButtonSliderState {
  uint8_t focus = 0;
  bool editing = false;
};

constexpr ReaderButtonSliderAction readerButtonSliderInput(ReaderButtonSliderState& state,
                                                           const ReaderButtonSliderInput input) {
  if (input == ReaderButtonSliderInput::Back) {
    if (state.editing) {
      state.editing = false;
      return ReaderButtonSliderAction::None;
    }
    return ReaderButtonSliderAction::LeavePane;
  }
  if (input == ReaderButtonSliderInput::Confirm) {
    state.editing = !state.editing;
    return ReaderButtonSliderAction::None;
  }
  if (state.editing) {
    return input == ReaderButtonSliderInput::Next ? ReaderButtonSliderAction::Increase
                                                  : ReaderButtonSliderAction::Decrease;
  }
  state.focus = input == ReaderButtonSliderInput::Next ? 1 : 0;
  return ReaderButtonSliderAction::None;
}

enum class ReaderDrawerCatalogItem : uint8_t {
  ReaderFont,
  TtfRendering,
  DictionaryFont,
  Spacing,
  TextAa,
  Focus,
  GuideDots,
  Margins,
  Orientation,
  Alignment,
  Hyphenation,
  PublisherPages,
  ExtraSpacing,
  ForceIndents,
  EmbeddedStyle,
  PublisherFonts,
  Images,
  ImageGrayscale,
  SelectChapter,
  GoToPercent,
  GoToStablePage,
  BookmarkToggle,
  ViewBookmarks,
  Screenshot,
  DisplayQr,
  Footnotes,
  Lookup,
  LookupHistory,
  SaveClipping,
  ViewClippings,
  DeleteBookmarks,
  StatusBar,
  BookDictionary,
  RenderMode,
  IndexingMethod,
  ToggleCompleted,
  ToggleArchived,
  CycleStatus,
  Controls,
  ResetReadingPace,
  DeleteCache,
  DeleteStats,
  AutoPageTurn,
  FontFamily,
  FontSize,
  CharacterSpacing,
  DictionaryFontFamily,
  DictionaryFontSize,
  TtfHinting,
  TtfRaster,
  TtfInterpreter,
  TtfWeight,
  TtfSlant,
  TtfStemDarkening,
  TtfReset,
  ResetBookReaderSettings,
  ReadingStats,
  TrackBookStats,
  SyncProgress,
  // Missing from this catalog entirely until this merge -- the flat button-only menu had it
  // (MenuAction::BOOKFUSION_SYNC), but EpubReaderTouchMenuActivity's own pre-unification catalog
  // never did, so it was already unreachable on touch; once this catalog became BOTH devices'
  // only menu, omitting it would have made it unreachable on button devices too, a real
  // regression from the pre-merge button menu. Restored here, gated the same as the other
  // previously-button-only items below.
  BookFusionSync,
  NearbyPositionSync,
  SendNearbyBook,
};

struct ReaderDrawerAvailability {
  bool hasFootnotes = false;
  bool hasDictionary = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool showReadingPaceReset = false;
  bool hasStablePageNumbers = false;
  bool buttonDevice = false;
  // Gates ToggleCompleted/ToggleArchived vs CycleStatus in the Settings tab below, matching the
  // button-driven menu's own isAo3Book split -- an AO3 fic's Finished/Archived state is driven by its
  // 5-state reading-status cycle and its own index-aware Archive Fic/Restore instead.
  bool isAo3Book = false;
  bool globalStatsEnabled = true;
  bool bookStatsEnabled = true;
};

struct ReaderDrawerTabCatalog {
  std::array<ReaderDrawerCatalogItem, 13> items{};
  uint8_t count = 0;

  constexpr void add(const ReaderDrawerCatalogItem item) { items[count++] = item; }
};

using ReaderDrawerCatalog = std::array<ReaderDrawerTabCatalog, READER_DRAWER_TAB_COUNT>;

// Stable bit index per hideable row, for CrossPointSettings::readerMenuHiddenRows. Only the
// More/Location/Settings tabs' action rows are hideable -- Font/Layout tab items are settings
// pickers, not optional menu actions, and are never gated here. -1 means "always shown".
// Append new entries at the next free bit; never renumber or reuse one (an old settings.json
// stores the raw bits). Bits 0-30 are shared with InkCap's non-AO3 catalog; CycleStatus is this
// branch's own AO3-only addition, so it gets the next free bit (31) rather than one of those.
constexpr int readerDrawerItemVisibilityBit(const ReaderDrawerCatalogItem item) {
  switch (item) {
    // More tab.
    case ReaderDrawerCatalogItem::Lookup:
      return 0;
    case ReaderDrawerCatalogItem::LookupHistory:
      return 1;
    case ReaderDrawerCatalogItem::SelectChapter:
      return 2;
    case ReaderDrawerCatalogItem::GoToPercent:
      return 3;
    case ReaderDrawerCatalogItem::GoToStablePage:
      return 4;
    case ReaderDrawerCatalogItem::AutoPageTurn:
      return 5;
    case ReaderDrawerCatalogItem::ReadingStats:
      return 6;
    case ReaderDrawerCatalogItem::Footnotes:
      return 7;
    // Location tab.
    case ReaderDrawerCatalogItem::BookmarkToggle:
      return 8;
    case ReaderDrawerCatalogItem::ViewBookmarks:
      return 9;
    case ReaderDrawerCatalogItem::DeleteBookmarks:
      return 10;
    case ReaderDrawerCatalogItem::SaveClipping:
      return 11;
    case ReaderDrawerCatalogItem::ViewClippings:
      return 12;
    case ReaderDrawerCatalogItem::SyncProgress:
      return 13;
    case ReaderDrawerCatalogItem::BookFusionSync:
      return 14;
    case ReaderDrawerCatalogItem::NearbyPositionSync:
      return 15;
    case ReaderDrawerCatalogItem::SendNearbyBook:
      return 16;
    case ReaderDrawerCatalogItem::Screenshot:
      return 17;
    case ReaderDrawerCatalogItem::DisplayQr:
      return 18;
    // Settings tab.
    case ReaderDrawerCatalogItem::StatusBar:
      return 19;
    case ReaderDrawerCatalogItem::Controls:
      return 20;
    case ReaderDrawerCatalogItem::BookDictionary:
      return 21;
    case ReaderDrawerCatalogItem::RenderMode:
      return 22;
    case ReaderDrawerCatalogItem::IndexingMethod:
      return 23;
    case ReaderDrawerCatalogItem::ToggleCompleted:
      return 24;
    case ReaderDrawerCatalogItem::ToggleArchived:
      return 25;
    case ReaderDrawerCatalogItem::TrackBookStats:
      return 26;
    case ReaderDrawerCatalogItem::ResetReadingPace:
      return 27;
    case ReaderDrawerCatalogItem::DeleteCache:
      return 28;
    case ReaderDrawerCatalogItem::DeleteStats:
      return 29;
    case ReaderDrawerCatalogItem::ResetBookReaderSettings:
      return 30;
    case ReaderDrawerCatalogItem::CycleStatus:
      return 31;
    default:
      return -1;
  }
}

constexpr bool readerDrawerItemVisible(const ReaderDrawerCatalogItem item, const uint64_t hiddenRows) {
  const int bit = readerDrawerItemVisibilityBit(item);
  return bit < 0 || (hiddenRows & (1ULL << bit)) == 0;
}

constexpr ReaderDrawerCatalog makeReaderDrawerCatalog(const ReaderDrawerAvailability available,
                                                      const uint64_t hiddenRows = 0) {
  ReaderDrawerCatalog catalog{};
  // Font/Layout tabs never go through this -- only the action tabs below are user-hideable.
  auto addIfVisible = [hiddenRows](ReaderDrawerTabCatalog& tab, const ReaderDrawerCatalogItem item) {
    if (readerDrawerItemVisible(item, hiddenRows)) tab.add(item);
  };
  auto& font = catalog[static_cast<size_t>(ReaderDrawerTab::Font)];
  font.add(ReaderDrawerCatalogItem::ReaderFont);
#if CROSSINK_SCALABLE_FONTS
  font.add(ReaderDrawerCatalogItem::PublisherFonts);
#endif
  font.add(ReaderDrawerCatalogItem::DictionaryFont);
  font.add(ReaderDrawerCatalogItem::Spacing);
  font.add(ReaderDrawerCatalogItem::CharacterSpacing);
  font.add(ReaderDrawerCatalogItem::TextAa);
  font.add(ReaderDrawerCatalogItem::Focus);
  font.add(ReaderDrawerCatalogItem::GuideDots);

  auto& layout = catalog[static_cast<size_t>(ReaderDrawerTab::Layout)];
  layout.add(ReaderDrawerCatalogItem::Margins);
  layout.add(ReaderDrawerCatalogItem::Orientation);
  layout.add(ReaderDrawerCatalogItem::Alignment);
  layout.add(ReaderDrawerCatalogItem::Images);
  layout.add(ReaderDrawerCatalogItem::ImageGrayscale);
  layout.add(ReaderDrawerCatalogItem::Hyphenation);
  layout.add(ReaderDrawerCatalogItem::PublisherPages);
  layout.add(ReaderDrawerCatalogItem::ExtraSpacing);
  layout.add(ReaderDrawerCatalogItem::ForceIndents);
  layout.add(ReaderDrawerCatalogItem::EmbeddedStyle);

  auto& more = catalog[static_cast<size_t>(ReaderDrawerTab::More)];
  if (available.hasDictionary) {
    addIfVisible(more, ReaderDrawerCatalogItem::Lookup);
    addIfVisible(more, ReaderDrawerCatalogItem::LookupHistory);
  }
  addIfVisible(more, ReaderDrawerCatalogItem::SelectChapter);
  addIfVisible(more, ReaderDrawerCatalogItem::GoToPercent);
  if (available.hasStablePageNumbers) addIfVisible(more, ReaderDrawerCatalogItem::GoToStablePage);
  addIfVisible(more, ReaderDrawerCatalogItem::AutoPageTurn);
  if (available.buttonDevice && available.globalStatsEnabled && available.bookStatsEnabled)
    addIfVisible(more, ReaderDrawerCatalogItem::ReadingStats);
  if (available.hasFootnotes) addIfVisible(more, ReaderDrawerCatalogItem::Footnotes);

  auto& location = catalog[static_cast<size_t>(ReaderDrawerTab::Location)];
  addIfVisible(location, ReaderDrawerCatalogItem::BookmarkToggle);
  if (available.hasBookmarks) {
    addIfVisible(location, ReaderDrawerCatalogItem::ViewBookmarks);
    addIfVisible(location, ReaderDrawerCatalogItem::DeleteBookmarks);
  }
  addIfVisible(location, ReaderDrawerCatalogItem::SaveClipping);
  if (available.hasClippings) addIfVisible(location, ReaderDrawerCatalogItem::ViewClippings);
  // Previously button-device-only: an artifact of the old separate touch menu never
  // having had these, not a deliberate touch-UX choice (see readerDrawerItemVisibilityBit()).
  // The reader-menu-visibility veto above gives touch users an opt-out if they don't want them.
  addIfVisible(location, ReaderDrawerCatalogItem::SyncProgress);
  addIfVisible(location, ReaderDrawerCatalogItem::BookFusionSync);
  addIfVisible(location, ReaderDrawerCatalogItem::NearbyPositionSync);
  addIfVisible(location, ReaderDrawerCatalogItem::SendNearbyBook);
  addIfVisible(location, ReaderDrawerCatalogItem::Screenshot);
  addIfVisible(location, ReaderDrawerCatalogItem::DisplayQr);

  auto& settings = catalog[static_cast<size_t>(ReaderDrawerTab::Settings)];
  addIfVisible(settings, ReaderDrawerCatalogItem::StatusBar);
  addIfVisible(settings, ReaderDrawerCatalogItem::Controls);
  addIfVisible(settings, ReaderDrawerCatalogItem::BookDictionary);
  addIfVisible(settings, ReaderDrawerCatalogItem::RenderMode);
  addIfVisible(settings, ReaderDrawerCatalogItem::IndexingMethod);
  if (available.isAo3Book) {
    addIfVisible(settings, ReaderDrawerCatalogItem::CycleStatus);
  } else {
    addIfVisible(settings, ReaderDrawerCatalogItem::ToggleCompleted);
    addIfVisible(settings, ReaderDrawerCatalogItem::ToggleArchived);
  }
  if (available.globalStatsEnabled) addIfVisible(settings, ReaderDrawerCatalogItem::TrackBookStats);
  if (available.showReadingPaceReset) addIfVisible(settings, ReaderDrawerCatalogItem::ResetReadingPace);
  addIfVisible(settings, ReaderDrawerCatalogItem::DeleteCache);
  if (available.globalStatsEnabled && available.bookStatsEnabled)
    addIfVisible(settings, ReaderDrawerCatalogItem::DeleteStats);
  addIfVisible(settings, ReaderDrawerCatalogItem::ResetBookReaderSettings);
  return catalog;
}

constexpr bool readerDrawerStepChangesSettings(const ReaderDrawerPane pane) {
  return pane == ReaderDrawerPane::CharacterSpacing || pane == ReaderDrawerPane::Spacing ||
         pane == ReaderDrawerPane::Margins || pane == ReaderDrawerPane::AutoPageTurn;
}

// Only settings with an existing live text preview reserve sample space.
constexpr bool readerDrawerShowsSamplePreview(const ReaderDrawerPane pane, const ReaderDrawerTab tab,
                                              const ReaderDrawerCatalogItem option) {
  return (pane == ReaderDrawerPane::Root && tab == ReaderDrawerTab::Font) || pane == ReaderDrawerPane::ReaderFont ||
         pane == ReaderDrawerPane::FontFamily || pane == ReaderDrawerPane::CharacterSpacing ||
         pane == ReaderDrawerPane::Spacing || pane == ReaderDrawerPane::Margins ||
         (pane == ReaderDrawerPane::EnumOptions &&
          (option == ReaderDrawerCatalogItem::FontSize || option == ReaderDrawerCatalogItem::Alignment));
}

constexpr bool readerDrawerSliderPreviewsText(const ReaderDrawerPane pane) {
  return pane == ReaderDrawerPane::CharacterSpacing || pane == ReaderDrawerPane::Spacing ||
         pane == ReaderDrawerPane::Margins;
}

// A landscape body is too short to stack a readable sample above the controls,
// so every preview pane puts the sample in a full-height column beside them.
constexpr bool readerDrawerSamplePreviewBesideControls(const bool landscape) { return landscape; }

constexpr bool shouldRenderReaderDrawerAntiAliasing(const bool previewRendered, const bool textAntiAliasing,
                                                    const bool foregroundBlack) {
  return previewRendered && textAntiAliasing && foregroundBlack;
}

// Panes with more vertical content than a single control row: Spacing/Margins place
// two annotated sliders, and Percent/StablePage place a 4-row numeric keypad. Keep
// their extra landscape height separate from the compact list panes.
constexpr bool readerDrawerNeedsTallLandscapeSheet(const ReaderDrawerPane pane) {
  return readerDrawerSliderPreviewsText(pane) || pane == ReaderDrawerPane::Percent ||
         pane == ReaderDrawerPane::StablePage;
}

constexpr bool readerDrawerNeedsExternalBackdrop(const bool previewDirty, const bool previewModelValid,
                                                 const bool activeFontChanged) {
  return !previewDirty || (!previewModelValid && !activeFontChanged);
}

constexpr bool isReaderDrawerRowFocused(const bool buttonFocusActive, const int16_t selectedIndex,
                                        const int16_t rowIndex) {
  return buttonFocusActive && selectedIndex == rowIndex;
}

constexpr int16_t readerDrawerFocusedWindowIndex(const bool buttonFocusActive, const int16_t selectedIndex,
                                                 const int16_t topIndex) {
  return buttonFocusActive ? static_cast<int16_t>(selectedIndex - topIndex) : -1;
}

constexpr int readerDrawerVisibleRows(const int16_t listHeight, const int16_t rowHeight, const int16_t rowGap) {
  return rowHeight + rowGap > 0 ? (listHeight + rowGap) / (rowHeight + rowGap) : 1;
}

constexpr int16_t readerDrawerListHeightForRows(const int rows, const int16_t rowHeight, const int16_t rowGap) {
  if (rows <= 0) return 0;
  return static_cast<int16_t>(rows * rowHeight + (rows - 1) * rowGap);
}

struct ReaderDrawerState {
  ReaderDrawerTab tab = ReaderDrawerTab::Font;
  ReaderDrawerPane pane = ReaderDrawerPane::Root;
  int16_t selectedIndex = 0;
  std::array<int16_t, READER_DRAWER_TAB_COUNT> rootTopIndex{};
  int16_t paneTopIndex = 0;
  int16_t pendingFontIndex = -1;
};

inline ReaderDrawerState initialReaderDrawerState(const bool hasTouchHardware) {
  ReaderDrawerState state;
  state.tab = hasTouchHardware ? ReaderDrawerTab::Font : ReaderDrawerTab::More;
  return state;
}

inline void restoreReaderDrawerScroll(ReaderDrawerState& state, const int16_t scrollPosition) {
  if (state.pane == ReaderDrawerPane::Root) {
    state.rootTopIndex[static_cast<size_t>(state.tab)] = scrollPosition;
  } else {
    state.paneTopIndex = scrollPosition;
  }
}

struct ReaderSettingsDraft {
  uint8_t fontFamily = 0;
  uint8_t readerFontPointSize = 0;
  std::array<char, 64> sdFontFamilyName{};
  uint8_t lineHeightPercent = 0;
  uint8_t wordSpacing = 0;
  uint8_t characterSpacing = 5;
  uint8_t screenMarginVertical = 0;
  uint8_t screenMarginHorizontal = 0;
  uint8_t orientation = 0;
  uint8_t paragraphAlignment = 0;
  uint8_t textAntiAliasing = 0;
  uint8_t focusReadingEnabled = 0;
  uint8_t guideReadingEnabled = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t publisherPageNumbers = 0;
  uint8_t extraParagraphSpacing = 0;
  uint8_t forceParagraphIndents = 0;
  uint8_t embeddedStyle = 0;
  uint8_t publisherFonts = 0;
  uint8_t imageRendering = 0;
  uint8_t imageGrayscale = 1;
  uint8_t epubRenderMode = 0;
  uint8_t indexingMethod = 0;
};

inline void restoreReaderDraftFont(ReaderSettingsDraft& draft, const ReaderSettingsDraft& lastGood) {
  draft.fontFamily = lastGood.fontFamily;
  draft.readerFontPointSize = lastGood.readerFontPointSize;
  draft.sdFontFamilyName = lastGood.sdFontFamilyName;
}

enum class ReaderSettingsChangeMask : uint8_t {
  None = 0,
  Preview = 1 << 0,
  Relayout = 1 << 1,
  Orientation = 1 << 2,
  NonLayout = 1 << 3,
};

constexpr ReaderSettingsChangeMask operator|(const ReaderSettingsChangeMask left,
                                             const ReaderSettingsChangeMask right) {
  return static_cast<ReaderSettingsChangeMask>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr bool hasReaderSettingsChange(const ReaderSettingsChangeMask mask, const ReaderSettingsChangeMask change) {
  return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(change)) != 0;
}
