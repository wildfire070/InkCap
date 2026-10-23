#include "ReaderMenuVisibilityActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Catalog order == bit order (readerDrawerItemVisibilityBit() in EpubReaderMenuModel.h);
// also the row's StrId. Font/Layout tab items are never listed: they're settings pickers,
// not optional actions, so readerDrawerItemVisibilityBit() never assigns them a bit.
struct VisibilityRow {
  ReaderDrawerCatalogItem item;
  StrId label;
  StrId sectionHeading;  // NONE on every row but the first of its drawer-tab group
};
constexpr StrId NONE = StrId::_COUNT;
constexpr VisibilityRow VISIBILITY_ROWS[] = {
    // More tab.
    {ReaderDrawerCatalogItem::Lookup, StrId::STR_LOOKUP, StrId::STR_MORE},
    {ReaderDrawerCatalogItem::LookupHistory, StrId::STR_LOOKUP_HISTORY, NONE},
    {ReaderDrawerCatalogItem::SelectChapter, StrId::STR_SELECT_CHAPTER, NONE},
    {ReaderDrawerCatalogItem::GoToPercent, StrId::STR_GO_TO_PERCENT, NONE},
    {ReaderDrawerCatalogItem::GoToStablePage, StrId::STR_GO_TO_STABLE_PAGE, NONE},
    {ReaderDrawerCatalogItem::AutoPageTurn, StrId::STR_AUTO_TURN_INTERVAL_SECONDS, NONE},
    {ReaderDrawerCatalogItem::ReadingStats, StrId::STR_READING_STATS, NONE},
    {ReaderDrawerCatalogItem::Footnotes, StrId::STR_FOOTNOTES, NONE},
    // Location tab.
    {ReaderDrawerCatalogItem::BookmarkToggle, StrId::STR_ADD_BOOKMARK, StrId::STR_LOCATION_SECTION},
    {ReaderDrawerCatalogItem::ViewBookmarks, StrId::STR_VIEW_BOOKMARKS, NONE},
    {ReaderDrawerCatalogItem::DeleteBookmarks, StrId::STR_DELETE_BOOKMARKS, NONE},
    {ReaderDrawerCatalogItem::SaveClipping, StrId::STR_SAVE_CLIPPING, NONE},
    {ReaderDrawerCatalogItem::ViewClippings, StrId::STR_VIEW_CLIPPINGS, NONE},
    {ReaderDrawerCatalogItem::SyncProgress, StrId::STR_SYNC_BOOK, NONE},
    {ReaderDrawerCatalogItem::BookFusionSync, StrId::STR_BF_SYNC, NONE},
    {ReaderDrawerCatalogItem::NearbyPositionSync, StrId::STR_NEARBY_POSITION_SYNC, NONE},
    {ReaderDrawerCatalogItem::SendNearbyBook, StrId::STR_SEND_NEARBY_BOOK, NONE},
    {ReaderDrawerCatalogItem::Screenshot, StrId::STR_SCREENSHOT_BUTTON, NONE},
    {ReaderDrawerCatalogItem::DisplayQr, StrId::STR_DISPLAY_QR, NONE},
    // Settings tab.
    {ReaderDrawerCatalogItem::StatusBar, StrId::STR_STATUS_BARS, StrId::STR_SETTINGS_SHORT},
    {ReaderDrawerCatalogItem::Controls, StrId::STR_CAT_CONTROLS, NONE},
    {ReaderDrawerCatalogItem::BookDictionary, StrId::STR_BOOK_DICTIONARY, NONE},
    {ReaderDrawerCatalogItem::RenderMode, StrId::STR_EPUB_RENDER_MODE, NONE},
    {ReaderDrawerCatalogItem::IndexingMethod, StrId::STR_INDEXING_METHOD, NONE},
    {ReaderDrawerCatalogItem::ToggleCompleted, StrId::STR_MARK_FINISHED, NONE},
    {ReaderDrawerCatalogItem::ToggleArchived, StrId::STR_ARCHIVE_FILE, NONE},
    // AO3-only: only one of this row / the ToggleCompleted+ToggleArchived pair above ever shows
    // for a given book (isAo3Book), but the hide decision itself isn't book-specific.
    {ReaderDrawerCatalogItem::CycleStatus, StrId::STR_BOOK_STATUS, NONE},
    {ReaderDrawerCatalogItem::TrackBookStats, StrId::STR_TRACK_READING_STATS, NONE},
    {ReaderDrawerCatalogItem::ResetReadingPace, StrId::STR_RESET_READING_PACE, NONE},
    {ReaderDrawerCatalogItem::DeleteCache, StrId::STR_DELETE_CACHE, NONE},
    {ReaderDrawerCatalogItem::DeleteStats, StrId::STR_DELETE_BOOK_STATS, NONE},
    {ReaderDrawerCatalogItem::ResetBookReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS, NONE},
};
constexpr size_t VISIBILITY_ROW_COUNT = sizeof(VISIBILITY_ROWS) / sizeof(VISIBILITY_ROWS[0]);
}  // namespace

ReaderMenuVisibilityActivity::ReaderMenuVisibilityActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReaderMenuVisibility", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {
  static_assert(VISIBILITY_ROW_COUNT == 32, "VISIBILITY_ROWS must list every hideable catalog item exactly once");
  for (size_t i = 0; i < VISIBILITY_ROW_COUNT; i++) rows[i] = VISIBILITY_ROWS[i].item;
  rowCount = VISIBILITY_ROW_COUNT;
}

void ReaderMenuVisibilityActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderMenuVisibilityActivity*>(user);
  if (event.value < 0 || static_cast<size_t>(event.value) >= self->rowCount) return;
  {
    // listNav and the viewport are render-owned; the render task may be mid-pass on S3.
    RenderLock lock(*self);
    self->selectedIndex = static_cast<size_t>(event.value);
    self->listNav.selected = event.value;
    self->topIndex = self->listNav.top;
  }
  self->app.clearTapFlash();
  self->toggleRow(self->selectedIndex);
}

void ReaderMenuVisibilityActivity::onEnter() {
  Activity::onEnter();

  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  selectedIndex = 0;
  listNav.selected = 0;
  listNav.top = 0;
  uiReady = false;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &ReaderMenuVisibilityActivity::onRowEvent, this);
  app.setScreen(&ReaderMenuVisibilityActivity::listScreen, this);
  requestUpdate();
}

void ReaderMenuVisibilityActivity::onExit() { Activity::onExit(); }

void ReaderMenuVisibilityActivity::moveSelection(const size_t next) {
  {
    RenderLock lock(*this);
    selectedIndex = next;
    listNav.selected = static_cast<int>(next);
    listNav.top = topIndex;
    listNav.follow(static_cast<int>(rowCount));
    topIndex = listNav.top;
  }
  requestUpdate();
}

void ReaderMenuVisibilityActivity::toggleRow(const size_t row) {
  if (row >= rowCount) return;
  const int bit = readerDrawerItemVisibilityBit(rows[row]);
  if (bit < 0) return;  // shouldn't happen: every listed row has a bit
  SETTINGS.readerMenuHiddenRows ^= (uint64_t{1} << bit);
  SETTINGS.saveToFile();
  requestUpdate();
}

void ReaderMenuVisibilityActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleRow(selectedIndex);
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    {
      RenderLock lock(*this);
      listNav.top = topIndex;
      const int page = listNav.pageRowsFor(static_cast<int>(rowCount));
      listNav.scrollBy(swipe == MappedInputManager::SwipeDir::Up ? page : -page, static_cast<int>(rowCount));
      topIndex = listNav.top;
    }
    requestUpdate();
    return;
  }

  buttonNavigator.onNext([this] { moveSelection((selectedIndex + 1) % rowCount); });
  buttonNavigator.onPrevious([this] { moveSelection((selectedIndex + rowCount - 1) % rowCount); });
}

void ReaderMenuVisibilityActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<ReaderMenuVisibilityActivity*>(user)->buildListScreen(screen);
}

void ReaderMenuVisibilityActivity::provideRow(void* user, const uint16_t row, fui::ListItem& item) {
  static_cast<ReaderMenuVisibilityActivity*>(user)->fillRow(row, item);
}

void ReaderMenuVisibilityActivity::fillRow(const uint16_t row, fui::ListItem& item) {
  const VisibilityRow& info = VISIBILITY_ROWS[row];
  item.label = I18N.get(info.label);
  item.actionValue = static_cast<int16_t>(row);
  if (info.sectionHeading != NONE) item.sectionHeading = I18N.get(info.sectionHeading);
  item.toggle = true;
  const int bit = readerDrawerItemVisibilityBit(info.item);
  item.toggleChecked = bit < 0 || (SETTINGS.readerMenuHiddenRows & (uint64_t{1} << bit)) == 0;
}

void ReaderMenuVisibilityActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput)), 0,
                                 static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.rowProvider = &ReaderMenuVisibilityActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rowCount);
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  configureUiList(props, screen.theme(), screen.body());
  props.headerText = screen.theme().bodyText;
  props.headerText.bold = true;
  configureUiListSectionHeaders(props, screen.theme());
  listNav.selected = static_cast<int>(selectedIndex);
  listNav.top = topIndex;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, static_cast<int>(rowCount), props);
  topIndex = listNav.top;
  screen.list(props);
}

void ReaderMenuVisibilityActivity::render(RenderLock&&) {
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  uiReady = false;
  // A section heading can push the selection off the measured page; rebuild until stable.
  for (int pass = 0; pass < 4; ++pass) {
    renderer.clearScreen();
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_READER_MENU), false);
    } else {
      GUI.drawHeader(renderer, header, tr(STR_READER_MENU));
    }
    renderUiApp(app, uiTarget);
    topIndex = listNav.top;
    if (!listNav.consumeRebuildNeeded()) break;
  }
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
