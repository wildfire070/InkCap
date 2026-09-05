#include "BookFusionBrowserActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <strings.h>

#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_PREV_PAGE = 2;
constexpr fui::ActionId ACTION_NEXT_PAGE = 3;
// Long-press threshold for the non-touch page-turn entry point: Left/Right
// otherwise scroll the list one row at a time (see loop()), so holding past
// this jumps a whole page instead. Same value File Browser's own long-press
// gestures use.
constexpr unsigned long PAGE_TURN_LONG_PRESS_MS = 600;
// PageForward's short-press action (Sort, or nothing on CATEGORY_SELECTION)
// fires on release rather than immediately, so a hold has a chance to become
// the long-press Search trigger instead -- see
// BookFusionBrowserActivity::updatePageForwardSortAndSearch(). Same threshold
// as the Left/Right page-turn long-press above, for consistency.
constexpr unsigned long SEARCH_LONG_PRESS_MS = 600;
// How far from the right edge row content (and screen.list()'s own row
// highlight background) is pulled in from the Sort/Search edge tabs (see
// render()). Chosen by eye against the device screen, matching
// FileBrowserActivity's identical kListRightClearance value.
constexpr int16_t kListRightClearance = 15;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
constexpr size_t DOWNLOAD_BUFFER_SIZE = 2048;

struct Category {
  StrId nameId;
  const char* list;  // BookFusion "list" filter, nullptr for "all books".
  UIIcon icon;
  const char* sort = nullptr;  // BookFusion sort key; nullptr defaults to "added_at-desc".
};

// list values match BookFusion's own book-list filters. Icons match
// InsiderPhD's fork's category-row icons (one per fixed category; shelves
// get UIIcon::Folder as the visual delimiter between "categories" and
// "shelves", same as there -- see buildCategoryScreen()). Currently Reading
// sorts by last-read time rather than the default added-date, so the book
// you're actually reading right now surfaces first, matching InsiderPhD's
// fork -- every other category leaves sort at its default.
constexpr Category CATEGORIES[] = {
    {StrId::STR_BF_CURRENTLY_READING, "currently_reading", UIIcon::Book, "last_read_at-desc"},
    {StrId::STR_BF_FAVORITES, "favorites", UIIcon::Star},
    {StrId::STR_BF_PLAN_TO_READ, "planned_to_read", UIIcon::Arrow},
    {StrId::STR_BF_COMPLETED, "completed", UIIcon::Check},
    {StrId::STR_BF_ALL_BOOKS, nullptr, UIIcon::Files},
};
constexpr int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

std::string buildBookFilename(const BookFusionBook& book) {
  if (book.author.empty()) return book.title;
  if (book.title.empty()) return book.author;
  return book.title + " - " + book.author;
}

// The device only has an EPUB reader (FileBrowserActivity's own allow-list is
// EPUB/XTC/TXT/MD/BMP). Other BookFusion formats (PDF, audio, etc.) appear in
// search results but can't be opened here, so those rows are disabled and a
// direct download attempt is refused rather than silently mis-saving one
// with a ".epub" extension it doesn't have.
bool bookFusionFormatIsEpub(const BookFusionBook& book) {
  return book.format.empty() || strcasecmp(book.format.c_str(), "epub") == 0;
}

// Image-heavy EPUBs strain the C3's ~380KB RAM and can slow or crash the
// renderer. Above this size, confirm before spending the download on it.
constexpr uint32_t LARGE_BOOK_WARN_BYTES = 10u * 1024 * 1024;  // 10 MB
bool bookFusionBookIsLarge(const BookFusionBook& book) { return book.downloadSize >= LARGE_BOOK_WARN_BYTES; }

// Same path construction downloadBook() uses to save the file, so the
// "already downloaded" row icon (see buildBrowsingScreen()) reflects exactly
// what a download would do, not an approximation of it.
std::string resolveBookFilePath(const BookFusionBook& book) {
  const char* downloadFolder = SETTINGS.bookFusionDownloadFolder;
  std::string path;
  path.reserve(96);
  if (downloadFolder[0] != '\0') path += downloadFolder;
  path += '/';
  path += StringUtils::sanitizeFilename(buildBookFilename(book));
  path += ".epub";
  return path;
}
}  // namespace

BookFusionBrowserActivity::BookFusionBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookFusionBrowser", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {
  // Rebind just this activity's small-font slot to a genuinely smaller face
  // (matches InsiderPhD's fork's SMALL_FONT_ID vs UI_10_FONT_ID size
  // contrast for book-row author lines and the page indicator).
  // makeUiTarget()'s app-wide default binds FONT_SMALL to the same font as
  // FONT_BODY (see UIScale.h's documented "list labels and their values
  // have the same visible size" convention) -- each activity owns its own
  // GfxRendererTarget, so rebinding here doesn't affect any other screen.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
}

void BookFusionBrowserActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  state = BrowserState::CHECK_WIFI;
  selectedCategory = 0;
  currentCategory = 0;
  bookshelves = BookFusionBookshelfList{};
  bookshelvesLoaded = false;
  currentBookshelfId = 0;
  currentBookshelfName.clear();
  page = BookFusionSearchResult{};
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  uiReady = false;
  visibleRows = 1;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &BookFusionBrowserActivity::onRowEvent, this);
  app.on(ACTION_PREV_PAGE, &BookFusionBrowserActivity::onPageButtonEvent, this);
  app.on(ACTION_NEXT_PAGE, &BookFusionBrowserActivity::onPageButtonEvent, this);
  app.setScreen(&BookFusionBrowserActivity::rootScreen, this);
  requestUpdate();

  if (!BookFusionSyncClient::getBearerToken().empty()) {
    // Not calling beginSession() here: keeping the BookFusion TLS connection
    // open for the whole browse session held its buffers in heap
    // continuously (see SecureHttpClient's keep-alive comment), permanently
    // taxing every subsequent page load/download instead of paying a brief,
    // fully-released cost per request. Each call now uses its own
    // short-lived fallback connection (see resolveClient()).
    checkAndConnectWifi();
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_BF_NO_TOKEN_MSG);
    requestUpdate();
  }
}

void BookFusionBrowserActivity::onExit() {
  Activity::onExit();
  BookFusionSyncClient::endSession();
  page = BookFusionSearchResult{};

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

bool BookFusionBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
      return true;
    case BrowserState::CATEGORY_SELECTION:
    case BrowserState::BROWSING:
    case BrowserState::DOWNLOAD_COMPLETE:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

// Total pages if BookFusion returned a Total-Count header, else 0 (unknown)
// -- matches InsiderPhD's fork's fallback of leaving wrap-to-last-page as a
// no-op when the total isn't known.
namespace {
int totalPagesFor(const BookFusionSearchResult& page) {
  return page.totalCount > 0 ? (page.totalCount + BOOKFUSION_BOOKS_PER_PAGE - 1) / BOOKFUSION_BOOKS_PER_PAGE : 0;
}
}  // namespace

void BookFusionBrowserActivity::activateSelected() {
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(page.books.size())) return;
  const BookFusionBook book = page.books[selectorIndex];  // copy: page/selectorIndex may change before the callback
  // Gate large image-heavy EPUBs behind a confirm screen -- the search API
  // gives us download_size up front, so we can ask before spending the
  // transfer. Non-EPUB formats are rejected inside downloadBook() directly,
  // same as InsiderPhD's fork's ordering (format gates the size check, not
  // the other way around -- no point warning about size for a file that
  // can't be opened at all).
  if (bookFusionFormatIsEpub(book) && bookFusionBookIsLarge(book)) {
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, book.title, tr(STR_BF_LARGE_BOOK_WARNING)),
        [this, book](const ActivityResult& result) {
          if (!result.isCancelled) downloadBook(book);
        });
    return;
  }
  downloadBook(book);
}

void BookFusionBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  self->app.clearTapFlash();

  if (self->state == BrowserState::CATEGORY_SELECTION) {
    if (event.value < 0 || event.value >= self->totalMenuRows()) return;
    self->selectCategory(event.value);
    return;
  }

  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int>(self->page.books.size())) return;
  self->selectorIndex = event.value;
  self->activateSelected();
}

void BookFusionBrowserActivity::onPageButtonEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  if (event.action == ACTION_PREV_PAGE && self->page.currentPage > 1) {
    self->loadPage(self->page.currentPage - 1);
  } else if (event.action == ACTION_NEXT_PAGE && self->page.hasMore) {
    self->loadPage(self->page.currentPage + 1);
  }
}

void BookFusionBrowserActivity::loop() {
  if (sortPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (state == BrowserState::WIFI_SELECTION) return;

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      checkAndConnectWifi();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::DOWNLOAD_COMPLETE) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      state = BrowserState::BROWSING;
      requestUpdate();
    }
    return;
  }

  if (state == BrowserState::CATEGORY_SELECTION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      selectCategory(selectedCategory);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }
    // Non-touch: long-press PageForward opens a global search from here (no
    // category/shelf filter), same button and threshold as BROWSING's Sort/
    // Search split -- PageForward's short-press does nothing on this screen
    // (allowSort=false), since Sort doesn't apply to the category list itself.
    if (!mappedInput.hasTouchHardware()) {
      if (updatePageForwardSortAndSearch(/*allowSort=*/false, /*launchesGlobalSearch=*/true)) return;
    }
    if (mappedInput.hasTouchHardware() && searchButtonRect.width > 0) {
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty) && tx >= searchButtonRect.x &&
          tx < searchButtonRect.x + searchButtonRect.width && ty >= searchButtonRect.y &&
          ty < searchButtonRect.y + searchButtonRect.height) {
        launchSearch(/*global=*/true);
        return;
      }
    }
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        if (app.invalidated()) requestUpdate();
        if (event) return;
      }
    }
    buttonNavigator.onNextRelease([this] {
      selectedCategory = ButtonNavigator::nextIndex(selectedCategory, totalMenuRows());
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selectedCategory = ButtonNavigator::previousIndex(selectedCategory, totalMenuRows());
      requestUpdate();
    });
    return;
  }

  if (state == BrowserState::BROWSING) {
    const int bookCount = static_cast<int>(page.books.size());

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::CATEGORY_SELECTION;
      requestUpdate();
      return;
    }

    // Non-touch: PageBack opens Sort directly, same as File Browser; PageForward
    // is shared between Sort (short-press) and Search (long-press) -- see
    // updatePageForwardSortAndSearch().
    if (!mappedInput.hasTouchHardware()) {
      if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
        openSortPopup();
        return;
      }
      if (updatePageForwardSortAndSearch(/*allowSort=*/true, /*launchesGlobalSearch=*/false)) return;
    }

    // Non-touch: holding Left/Right past the threshold jumps a whole page,
    // instead of their short-press row-scroll below. longPressPageTurnHandled
    // swallows the release that follows a hold that already fired, so it
    // doesn't also move the row selection.
    if (!mappedInput.hasTouchHardware() && !longPressPageTurnHandled) {
      if (page.currentPage > 1 && mappedInput.isPressed(MappedInputManager::Button::Left) &&
          mappedInput.getHeldTime() >= PAGE_TURN_LONG_PRESS_MS) {
        longPressPageTurnHandled = true;
        loadPage(page.currentPage - 1);
        return;
      }
      if (page.hasMore && mappedInput.isPressed(MappedInputManager::Button::Right) &&
          mappedInput.getHeldTime() >= PAGE_TURN_LONG_PRESS_MS) {
        longPressPageTurnHandled = true;
        loadPage(page.currentPage + 1);
        return;
      }
    }

    if (bookCount > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
      return;
    }

    if (mappedInput.hasTouchHardware() && sortButtonRect.width > 0) {
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty) && tx >= sortButtonRect.x && tx < sortButtonRect.x + sortButtonRect.width &&
          ty >= sortButtonRect.y && ty < sortButtonRect.y + sortButtonRect.height) {
        openSortPopup();
        return;
      }
    }

    if (mappedInput.hasTouchHardware() && searchButtonRect.width > 0) {
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty) && tx >= searchButtonRect.x &&
          tx < searchButtonRect.x + searchButtonRect.width && ty >= searchButtonRect.y &&
          ty < searchButtonRect.y + searchButtonRect.height) {
        launchSearch(/*global=*/false);
        return;
      }
    }

    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        if (app.invalidated()) requestUpdate();
        if (event) return;
        if (state != BrowserState::BROWSING) return;
      }
    }

    // Page-turn also stays touch-tappable via the fixed footer (see
    // buildBrowsingScreen()) through ACTION_PREV_PAGE/NEXT_PAGE -- only the
    // non-touch trigger moved, from PageBack/PageForward above to the
    // long-press on Left/Right below.

    // Up/Down/Left/Right stay bounded to the current page's books -- no falling
    // off either end into the adjacent page, since that's what page-turn above
    // is for. longPressPageTurnHandled swallows the release that follows a
    // Left/Right hold that already jumped a page (see above), so that release
    // doesn't also move the row selection.
    if (bookCount > 0) {
      const auto moveSelection = [this, bookCount](const int index) {
        selectorIndex = index;
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, bookCount);
        requestUpdate();
      };
      buttonNavigator.onNextRelease([this, &moveSelection, bookCount] {
        if (longPressPageTurnHandled) {
          longPressPageTurnHandled = false;
          return;
        }
        moveSelection(ButtonNavigator::nextIndex(selectorIndex, bookCount));
      });
      buttonNavigator.onPreviousRelease([this, &moveSelection, bookCount] {
        if (longPressPageTurnHandled) {
          longPressPageTurnHandled = false;
          return;
        }
        moveSelection(ButtonNavigator::previousIndex(selectorIndex, bookCount));
      });
    }
  }
}

void BookFusionBrowserActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::CATEGORY_SELECTION:
      self->buildCategoryScreen(screen);
      break;
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    case BrowserState::DOWNLOAD_COMPLETE:
      self->buildDownloadCompleteScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

void BookFusionBrowserActivity::screenHeader(UiApp::ScreenType& screen, const char* title) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  fui::HeaderProps header;
  header.title = title;
  header.borderEdges = fui::EdgeBottom;
  screen.header(header);
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void BookFusionBrowserActivity::buildCategoryScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BF_BROWSE_LIBRARY));

  std::vector<fui::ListItem> items;
  items.reserve(totalMenuRows());
  for (int i = 0; i < NUM_CATEGORIES; i++) {
    fui::ListItem item;
    item.label = I18N.get(CATEGORIES[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    item.icon = listIconFor(CATEGORIES[i].icon, 24);
    items.push_back(item);
  }
  if (bookshelvesLoaded) {
    for (size_t i = 0; i < bookshelves.shelves.size(); i++) {
      fui::ListItem item;
      item.label = bookshelves.shelves[i].name.c_str();
      item.actionValue = static_cast<int16_t>(NUM_CATEGORIES + i);
      // Folder is the visual delimiter between fixed categories (above,
      // each with its own icon) and the user's own shelves -- matches
      // InsiderPhD's fork rather than adding a separator row.
      item.icon = listIconFor(UIIcon::Folder, 24);
      items.push_back(item);
    }
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedCategory);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  // InkCap's ported Lyra theme (LyraTheme.h) sets listRowHeight=36, but
  // InsiderPhD's original Lyra rows are 40px -- a real 4px/row drift, not a
  // deliberate InkCap design choice. Match it here rather than in
  // LyraTheme.h itself, since that constant is shared by ~30 other list
  // screens app-wide and this is scoped to just this BookFusion menu.
  // Lyra_3_Covers/Carousel are InkCap-only variants with their own
  // independently-tuned row heights and no InsiderPhD equivalent to match,
  // so this only applies to plain Lyra.
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::LYRA) {
    props.rowHeight = 40;
  }
  // Clears the Search edge tab (see render()) so its row highlight background
  // doesn't run under it -- this screen always shows that tab.
  screen.insetContent(fui::Insets{0, kListRightClearance, 0, 0});
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  screen.list(props);
}

void BookFusionBrowserActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  // A global search (see performSearch()) has no category/shelf of its own --
  // currentCategory is -1 in that case, so CATEGORIES[] must not be indexed.
  std::string title;
  if (!activeSearchQuery.empty() && searchIsGlobal) {
    title = tr(STR_SEARCH);
  } else if (currentBookshelfId != 0) {
    title = currentBookshelfName;
  } else {
    title = I18N.get(CATEGORIES[currentCategory].nameId);
  }
  if (!activeSearchQuery.empty()) {
    title += ": \"";
    title += activeSearchQuery;
    title += "\"";
  }
  screenHeader(screen, title.c_str());

  if (page.books.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Footer strip, reserved from the bottom before the list claims the rest
  // of the body. Previous/Next live here rather than as rows inside the
  // scrollable list -- with 10 books per page they no longer fit alongside
  // the books without scrolling, and putting them in a fixed footer means
  // they're always reachable in one tap/button press regardless of scroll
  // position (see loop()'s PageBack/PageForward handling for the button
  // path). theme().smallText already carries the FONT_SMALL slot (a slot
  // index, not a raw font ID -- see the constructor's slot rebind).
  //
  // Touch: Prev Page (left, tappable) / "N / M" indicator (center) / Next
  // Page (right, tappable) -- unchanged layout, just a shorter Prev label so
  // it no longer truncates to "Previous P...".
  //
  // Non-touch: the tappable-looking Prev/Next boxes are actually
  // unreachable here -- Up/Down only ever move the list selection (see
  // loop()), never focus this footer -- so drawing them is misleading.
  // Instead the indicator moves to the left (out of "selectable row" shape)
  // and a small hint naming the real trigger (long-press Left/Right, see
  // loop()'s PAGE_TURN_LONG_PRESS_MS handling) sits on the right, roughly
  // over the Up/Down button hints at the very bottom of the screen.
  const auto& theme = screen.theme();
  const bool touch = mappedInput.hasTouchHardware();
  fui::TextStyle indicatorStyle = theme.smallText;
  indicatorStyle.align = fui::TextAlign::Center;
  const int16_t footerH = theme.rowHeight;
  const fui::Rect footerRect = screen.takeBottom(footerH);
  const int16_t navBtnW = static_cast<int16_t>(footerRect.width / 4);

  // Every page shows exactly its books -- no "Previous/Next Page" rows
  // mixed in (see loop()'s comment: those used to overflow the visible
  // rows once pages grew to 10 books, and don't exist in InsiderPhD's fork
  // at all, which pages by stepping off either end of the list instead).
  std::vector<fui::ListItem> items;
  items.reserve(page.books.size());
  for (size_t i = 0; i < page.books.size(); ++i) {
    const auto& book = page.books[i];
    fui::ListItem item;
    item.label = book.title.c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    item.actionValue = static_cast<int16_t>(i);
    // Check replaces the BookFusion mark once the file already exists
    // locally -- matches InsiderPhD's fork's per-row "already downloaded"
    // indicator, checked against the exact path a download would use.
    const bool alreadyOnSd = Storage.exists(resolveBookFilePath(book).c_str());
    item.icon = listIconFor(alreadyOnSd ? UIIcon::Check : UIIcon::BookFusion, 32);
    // Non-EPUB rows (PDF, audio, etc.) are disabled rather than struck
    // through -- InkCap's list widget has no per-row text-strike hook, and
    // disabling both dims the row and blocks touch activation outright.
    // downloadBook() also refuses these directly, since Confirm-button
    // activation bypasses a disabled row's touch gating.
    item.enabled = bookFusionFormatIsEpub(book);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  // InkCap deliberately keeps list labels and subtitles the same size
  // app-wide (UIScale.h: "so list labels and their values have the same
  // visible size") -- InsiderPhD's fork instead uses a visibly smaller font
  // for the author line (SMALL_FONT_ID vs UI_10_FONT_ID), which is what
  // reads as the title looking bold even though neither actually is. This
  // screen intentionally breaks from InkCap's own convention to match that.
  // theme().smallText already carries the FONT_SMALL slot, rebound to
  // SMALL_FONT_ID for this activity in the constructor.
  props.subtitleText = theme.smallText;
  // Clears the Sort/Search edge tabs (see render()) so the row highlight
  // background doesn't run under them -- this screen always shows Sort, and
  // Search once a category/shelf is open.
  screen.insetContent(fui::Insets{0, kListRightClearance, 0, 0});
  const auto rows = configureUiList(props, theme, screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(page.books.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);

  if (touch) {
    if (page.currentPage > 1) {
      fui::ButtonProps prevBtn;
      prevBtn.label = tr(STR_BF_PREV_PAGE_SHORT);
      prevBtn.action = ACTION_PREV_PAGE;
      screen.button(prevBtn, fui::Rect{footerRect.x, footerRect.y, navBtnW, footerRect.height});
    }
    if (page.hasMore) {
      fui::ButtonProps nextBtn;
      nextBtn.label = tr(STR_NEXT_PAGE);
      nextBtn.action = ACTION_NEXT_PAGE;
      screen.button(nextBtn, fui::Rect{static_cast<int16_t>(footerRect.x + footerRect.width - navBtnW), footerRect.y,
                                       navBtnW, footerRect.height});
    }
  }

  char indicator[24];
  if (page.totalCount > 0) {
    const int totalPages = totalPagesFor(page);
    snprintf(indicator, sizeof(indicator), "%d / %d", page.currentPage, totalPages);
  } else if (page.hasMore) {
    snprintf(indicator, sizeof(indicator), "%d / %d+", page.currentPage, page.currentPage);
  } else {
    snprintf(indicator, sizeof(indicator), "%d / %d", page.currentPage, page.currentPage);
  }

  if (touch) {
    const fui::Rect indicatorRect{static_cast<int16_t>(footerRect.x + navBtnW), footerRect.y,
                                  static_cast<int16_t>(footerRect.width - navBtnW * 2), footerRect.height};
    screen.target().text(indicatorRect, indicator, indicatorStyle);
  } else {
    // Left edge lines up with the header title's own left inset, so "N / M"
    // reads as part of the same left column as "All Books" above it.
    fui::TextStyle leftStyle = indicatorStyle;
    leftStyle.align = fui::TextAlign::Left;
    const int16_t leftPad = theme.headerSidePadding;
    const fui::Rect indicatorRect{static_cast<int16_t>(footerRect.x + leftPad), footerRect.y,
                                  static_cast<int16_t>(footerRect.width / 2), footerRect.height};
    screen.target().text(indicatorRect, indicator, leftStyle);

    if (page.currentPage > 1 || page.hasMore) {
      // Asks the *active* theme where it actually put the "Up"/"Down" hint
      // boxes (index 2/3, matching MappedInputManager::mapLabels' previous/
      // next slots) rather than assuming one theme's layout -- Base, Lyra,
      // Minimal, and RoundedRaff each place/size these differently (see
      // each theme's own buttonHintRect()).
      const Rect upRect = GUI.buttonHintRect(renderer, 2);
      const Rect downRect = GUI.buttonHintRect(renderer, 3);
      const int16_t upCenter = static_cast<int16_t>(upRect.x + upRect.width / 2);
      const int16_t downCenter = static_cast<int16_t>(downRect.x + downRect.width / 2);

      fui::TextStyle hintTextStyle = indicatorStyle;
      hintTextStyle.align = fui::TextAlign::Left;
      const char* prefixText = tr(STR_BF_PAGE_HOLD_PREFIX);
      const int16_t prefixWidth =
          static_cast<int16_t>(screen.target().measureText(indicatorStyle.font, prefixText, indicatorStyle).width);
      constexpr int16_t kHoldPrefixGap = 4;

      const auto drawLeftAt = [&](int16_t x, const char* text) -> int16_t {
        const int16_t width =
            static_cast<int16_t>(screen.target().measureText(indicatorStyle.font, text, indicatorStyle).width);
        screen.target().text(fui::Rect{x, footerRect.y, static_cast<int16_t>(width + 1), footerRect.height}, text,
                             hintTextStyle);
        return width;
      };

      if (page.currentPage > 1 && page.hasMore) {
        // "Prev Page | Next Page" -- the "|" glyph itself is centered
        // exactly midway between the Up/Down button centers; "Prev Page"
        // and "Next Page" fall out to either side of it rather than the
        // whole string being centered as one block (their widths aren't
        // guaranteed equal in a proportional font). The gap on each side of
        // "|" is an explicit constant, not the string's own embedded
        // spaces -- measureText's bounding box doesn't count a leading/
        // trailing space's advance width the same way drawText renders it,
        // which left the two gaps visibly uneven.
        std::string full = I18n::getInstance().get(StrId::STR_BF_PAGE_HOLD_HINT_BOTH);
        const size_t pipePos = full.find('|');
        std::string leftPart = pipePos == std::string::npos ? full : full.substr(0, pipePos);
        std::string rightPart = pipePos == std::string::npos ? "" : full.substr(pipePos + 1);
        while (!leftPart.empty() && leftPart.back() == ' ') leftPart.pop_back();
        while (!rightPart.empty() && rightPart.front() == ' ') rightPart.erase(rightPart.begin());
        constexpr int16_t kPipeSideGap = 4;
        const int16_t midX = static_cast<int16_t>((upCenter + downCenter) / 2);
        const int16_t pipeWidth =
            static_cast<int16_t>(screen.target().measureText(indicatorStyle.font, "|", indicatorStyle).width);
        const int16_t leftWidth = static_cast<int16_t>(
            screen.target().measureText(indicatorStyle.font, leftPart.c_str(), indicatorStyle).width);
        const int16_t pipeX = static_cast<int16_t>(midX - pipeWidth / 2);
        const int16_t leftX = static_cast<int16_t>(pipeX - kPipeSideGap - leftWidth);
        const int16_t rightX = static_cast<int16_t>(pipeX + pipeWidth + kPipeSideGap);
        const int16_t prefixX = static_cast<int16_t>(leftX - kHoldPrefixGap - prefixWidth);
        drawLeftAt(prefixX, prefixText);
        drawLeftAt(leftX, leftPart.c_str());
        drawLeftAt(pipeX, "|");
        drawLeftAt(rightX, rightPart.c_str());
      } else {
        const StrId directionId = page.currentPage > 1 ? StrId::STR_BF_PREV_PAGE_SHORT : StrId::STR_NEXT_PAGE;
        const int16_t buttonCenterX = page.currentPage > 1 ? upCenter : downCenter;
        const char* directionText = I18n::getInstance().get(directionId);
        const int16_t directionWidth = static_cast<int16_t>(
            screen.target().measureText(indicatorStyle.font, directionText, indicatorStyle).width);
        const int16_t directionX = static_cast<int16_t>(buttonCenterX - directionWidth / 2);
        const int16_t prefixX = static_cast<int16_t>(directionX - kHoldPrefixGap - prefixWidth);
        drawLeftAt(prefixX, prefixText);
        drawLeftAt(directionX, directionText);
      }
    }
  }
}

void BookFusionBrowserActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  // Keep the category/shelf name visible instead of switching to a generic
  // "Downloading" bar -- matches InsiderPhD's fork, which never replaces the
  // browsing screen's own header for this. Avoids the previous mismatch on
  // the complete screen too, which kept saying "Downloading..." after the
  // transfer had already finished.
  screenHeader(screen,
               currentBookshelfId != 0 ? currentBookshelfName.c_str() : I18N.get(CATEGORIES[currentCategory].nameId));

  const auto& theme = screen.theme();
  fui::TextStyle centeredBold = theme.bodyText;
  centeredBold.align = fui::TextAlign::Center;
  centeredBold.bold = true;
  fui::TextStyle centeredRegular = theme.bodyText;
  centeredRegular.align = fui::TextAlign::Center;

  const int16_t lh = screen.target().lineHeight(centeredRegular.font);
  const int16_t gap = theme.spaceMd;

  // Static info card (title + author + filesize/estimate + status), no live
  // progress bar -- matches InsiderPhD's fork's download screen rather than
  // InkCap's previous progress-bar layout.
  char titleLine[192];
  snprintf(titleLine, sizeof(titleLine), tr(STR_DOWNLOAD_TITLE_FORMAT), downloadTitle.c_str());
  const bool haveAuthor = !downloadAuthor.empty();
  char authorLine[192];
  if (haveAuthor) snprintf(authorLine, sizeof(authorLine), tr(STR_DOWNLOAD_AUTHOR_FORMAT), downloadAuthor.c_str());

  const bool haveSize = downloadTotal > 0;
  char sizeLine[64];
  if (haveSize) {
    // Matches InsiderPhD's fork's estimate constant -- a measured throughput
    // figure for this hardware/network class, not a live transfer rate.
    constexpr size_t kEstimatedBytesPerSec = 30 * 1024;
    const unsigned estSec =
        static_cast<unsigned>((downloadTotal + kEstimatedBytesPerSec - 1) / kEstimatedBytesPerSec);
    char sizeStr[16];
    snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", downloadTotal / (1024.0f * 1024.0f));
    if (estSec > 90) {
      snprintf(sizeLine, sizeof(sizeLine), tr(STR_DOWNLOAD_SIZE_MIN_FORMAT), sizeStr, (estSec + 59) / 60);
    } else {
      snprintf(sizeLine, sizeof(sizeLine), tr(STR_DOWNLOAD_SIZE_SEC_FORMAT), sizeStr, estSec);
    }
  }

  const int16_t blockH = static_cast<int16_t>(lh + gap + (haveAuthor ? lh + gap : 0) + (haveSize ? lh + gap : 0) + lh);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), titleLine, centeredBold);
  if (haveAuthor) screen.target().text(screen.takeTop(lh, gap), authorLine, centeredRegular);
  if (haveSize) screen.target().text(screen.takeTop(lh, gap), sizeLine, centeredRegular);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centeredBold);
}

void BookFusionBrowserActivity::buildDownloadCompleteScreen(UiApp::ScreenType& screen) {
  // Keep the category/shelf name visible instead of switching to a generic
  // "Downloading" bar -- matches InsiderPhD's fork, which never replaces the
  // browsing screen's own header for this. Avoids the previous mismatch on
  // the complete screen too, which kept saying "Downloading..." after the
  // transfer had already finished.
  screenHeader(screen,
               currentBookshelfId != 0 ? currentBookshelfName.c_str() : I18N.get(CATEGORIES[currentCategory].nameId));

  const auto& theme = screen.theme();
  fui::TextStyle centeredBold = theme.bodyText;
  centeredBold.align = fui::TextAlign::Center;
  centeredBold.bold = true;
  fui::TextStyle centeredRegular = theme.bodyText;
  centeredRegular.align = fui::TextAlign::Center;

  const int16_t lh = screen.target().lineHeight(centeredRegular.font);
  const int16_t gap = theme.spaceMd;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + gap);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_BF_DOWNLOAD_COMPLETE), centeredBold);
  screen.target().text(screen.takeTop(lh, gap), downloadTitle.c_str(), centeredRegular);
}

void BookFusionBrowserActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, tr(STR_BF_BROWSE_LIBRARY));

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  screen.centeredText(statusMessage.c_str(), centered);
}

void BookFusionBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::CATEGORY_SELECTION:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case BrowserState::BROWSING:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::DOWNLOAD_COMPLETE:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  app.render();
  uiReady = true;

  // Persistent "Sort"/"Search" edge tabs: visible on every device (not gated
  // on touch hardware) so a button-only reader can see they exist at all.
  // Non-touch triggers are PageBack (Sort) and a long-press of PageForward
  // (Search) -- see loop()'s updatePageForwardSortAndSearch(). These tabs are
  // the on-screen hint for that (and the touch tap targets). Sort only makes
  // sense once a category/shelf's book list is open; Search works from there
  // too (scoped to that list) or straight from the categories screen (global).
  {
    constexpr int kEdgeTabPadding = 6;
    constexpr int kEdgeTabWidth = 30;
    constexpr int kEdgeTabTopMargin = 4;
    constexpr int kEdgeTabCornerRadius = 6;
    constexpr int kEdgeTabGap = 4;  // between Sort and Search when both are stacked
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const auto drawEdgeTab = [&](const char* label, const int topY) {
      const int labelLen = renderer.getTextWidth(SMALL_FONT_ID, label);
      const int tabHeight = labelLen + kEdgeTabPadding * 2;
      const int tabX = pageWidth - kEdgeTabWidth;
      renderer.fillRoundedRect(tabX, topY, kEdgeTabWidth, tabHeight, kEdgeTabCornerRadius,
                               /*roundTopLeft=*/true, /*roundTopRight=*/false, /*roundBottomLeft=*/true,
                               /*roundBottomRight=*/false, Color::White);
      renderer.drawRoundedRect(tabX, topY, kEdgeTabWidth, tabHeight, 1, kEdgeTabCornerRadius,
                               /*roundTopLeft=*/true, /*roundTopRight=*/false, /*roundBottomLeft=*/true,
                               /*roundBottomRight=*/false, /*state=*/true);
      const int textX = tabX + (kEdgeTabWidth - renderer.getTextHeight(SMALL_FONT_ID)) / 2;
      const int textY = topY + (tabHeight + labelLen) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, label);
      return Rect{tabX, topY, kEdgeTabWidth, tabHeight};
    };

    const int topTabY = metrics.topPadding + metrics.headerHeight + kEdgeTabTopMargin;
    if (state == BrowserState::BROWSING) {
      sortButtonRect = drawEdgeTab(tr(STR_SORT), topTabY);
      searchButtonRect = drawEdgeTab(tr(STR_SEARCH), sortButtonRect.y + sortButtonRect.height + kEdgeTabGap);
    } else if (state == BrowserState::CATEGORY_SELECTION) {
      sortButtonRect = Rect{0, 0, 0, 0};
      searchButtonRect = drawEdgeTab(tr(STR_SEARCH), topTabY);
    } else {
      sortButtonRect = Rect{0, 0, 0, 0};
      searchButtonRect = Rect{0, 0, 0, 0};
    }
  }

  if (sortPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}

void BookFusionBrowserActivity::showLoadingBeforeFetch() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFBrowser", "Loading screen could not be rendered before fetch");
    requestUpdate(true);
  }
}

int BookFusionBrowserActivity::totalMenuRows() const {
  return NUM_CATEGORIES + (bookshelvesLoaded ? static_cast<int>(bookshelves.shelves.size()) : 0);
}

void BookFusionBrowserActivity::selectCategory(int index) {
  if (index < 0 || index >= totalMenuRows()) return;
  selectedCategory = index;
  // A search from a previous category/shelf (or a global one) shouldn't
  // silently carry over onto this newly picked one.
  activeSearchQuery.clear();
  searchIsGlobal = false;
  if (index < NUM_CATEGORIES) {
    currentCategory = index;
    currentBookshelfId = 0;
    currentBookshelfName.clear();
  } else {
    const size_t shelfIdx = static_cast<size_t>(index - NUM_CATEGORIES);
    if (shelfIdx >= bookshelves.shelves.size()) return;
    currentCategory = -1;
    currentBookshelfId = bookshelves.shelves[shelfIdx].id;
    currentBookshelfName = bookshelves.shelves[shelfIdx].name;
  }
  selectorIndex = 0;
  topIndex = 0;
  showLoadingBeforeFetch();
  loadPage(1);
}

void BookFusionBrowserActivity::openSortPopup() {
  // Whichever path opened this (touch tap, PageBack, or PageForward's own
  // guard expiry), a pending PageForward guard is now moot -- clear it so a
  // stale timestamp can't fire a second, unwanted open later.
  pendingSortFromPageForwardMs = 0;
  // tr(x) is a macro expanding to I18N.get(StrId::x) via token-pasting -- it can't take
  // a runtime variable, so this array/loop calls I18N.get() directly instead.
  static const StrId kFieldLabelIds[BOOKFUSION_SORT_FIELD_COUNT] = {
      StrId::STR_SORT_DATE, StrId::STR_SORT_LAST_READ, StrId::STR_SORT_AUTHOR, StrId::STR_TITLE};
  // Popup display order (Title, Author, Date, Last Read) -- deliberately
  // separate from the storage/API index above (SortField enum order, also
  // kFieldApiNames in loadPage()), so a value already saved to
  // SETTINGS.bookFusionSortField keeps meaning the same field after this
  // reorder rather than silently pointing at a different one.
  static constexpr SortField kDisplayOrder[BOOKFUSION_SORT_FIELD_COUNT] = {
      SortField::Title, SortField::Author, SortField::Date, SortField::LastRead};
  std::vector<std::string> labels;
  labels.reserve(BOOKFUSION_SORT_FIELD_COUNT);
  for (const SortField field : kDisplayOrder) {
    labels.emplace_back(I18N.get(kFieldLabelIds[static_cast<int>(field)]));
  }

  const uint8_t fieldRaw = SETTINGS.bookFusionSortField;
  int activeDisplayPos = -1;
  if (fieldRaw < BOOKFUSION_SORT_FIELD_COUNT) {
    for (int i = 0; i < BOOKFUSION_SORT_FIELD_COUNT; ++i) {
      if (kDisplayOrder[i] == static_cast<SortField>(fieldRaw)) {
        activeDisplayPos = i;
        break;
      }
    }
  }
  const bool ascending = SETTINGS.bookFusionSortAscending != 0;

  sortPopup.show(
      StrId::STR_SORT, std::move(labels), activeDisplayPos, ascending,
      [this](int displayPos, bool asc) {
        if (displayPos < 0 || displayPos >= BOOKFUSION_SORT_FIELD_COUNT) return;
        SETTINGS.bookFusionSortField = static_cast<uint8_t>(kDisplayOrder[displayPos]);
        SETTINGS.bookFusionSortAscending = asc ? 1 : 0;
        SETTINGS.saveToFile();
        selectorIndex = 0;
        topIndex = 0;
        showLoadingBeforeFetch();
        loadPage(1);
      },
      [this] { requestUpdate(); });
  // sortPopup.handleInput() (see loop()) runs before this state-specific branch
  // every frame, so it already saw active=false this frame and did nothing --
  // nothing else on this frame schedules a repaint. PageBack's/the old
  // PageForward's press-triggered open got this for free from the button's own
  // release moments later (aliased to the same pins ButtonNavigator treats as
  // row-nav, which handleInput()'s Up/Down branch explicitly requests one
  // for); this one fires exactly at release, so there's no leftover release
  // left to do that -- request explicitly instead of relying on one.
  requestUpdate();
}

bool BookFusionBrowserActivity::updatePageForwardSortAndSearch(const bool allowSort, const bool launchesGlobalSearch) {
  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    pendingSortFromPageForwardMs = millis();
    searchLongPressHandled = false;
  }
  // Consumed every loop(), not just while a press is pending: while the
  // screenshot chord is actively consuming input, main.cpp returns before this
  // Activity's loop() ever runs, so this flag (see its own comment) is the
  // only way this code can learn a chord happened across however many frames
  // it was frozen for -- isPressed(Power) alone can't, since by the time
  // loop() runs again Power has already been released.
  const bool chordJustConsumed = mappedInput.consumeScreenshotChordFlag();
  if (pendingSortFromPageForwardMs == 0) return false;

  if (chordJustConsumed || mappedInput.isPressed(MappedInputManager::Button::Power)) {
    pendingSortFromPageForwardMs = 0;  // Was a screenshot combo, not a PageForward request at all.
    searchLongPressHandled = false;
    return false;
  }

  if (!searchLongPressHandled && millis() - pendingSortFromPageForwardMs >= SEARCH_LONG_PRESS_MS) {
    searchLongPressHandled = true;
    pendingSortFromPageForwardMs = 0;
    launchSearch(launchesGlobalSearch);
    return true;
  }

  if (!mappedInput.isPressed(MappedInputManager::Button::PageForward)) {
    // Released before the long-press threshold, and not a screenshot combo --
    // a genuine short press.
    pendingSortFromPageForwardMs = 0;
    searchLongPressHandled = false;
    if (allowSort) {
      openSortPopup();
      return true;
    }
  }

  return false;
}

void BookFusionBrowserActivity::launchSearch(const bool global) {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), activeSearchQuery);
  startActivityForResult(std::move(keyboard), [this, global](const ActivityResult& result) {
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text, global);
    } else {
      requestUpdate();
    }
  });
}

void BookFusionBrowserActivity::performSearch(const std::string& query, const bool global) {
  if (query.empty()) {
    const bool wasGlobal = searchIsGlobal;
    activeSearchQuery.clear();
    searchIsGlobal = false;
    if (wasGlobal) {
      // A global search has no category/shelf of its own to fall back to --
      // land back on the categories screen rather than an arbitrary list.
      state = BrowserState::CATEGORY_SELECTION;
      requestUpdate();
      return;
    }
    if (state != BrowserState::BROWSING) {
      requestUpdate();
      return;
    }
    selectorIndex = 0;
    topIndex = 0;
    showLoadingBeforeFetch();
    loadPage(1);
    return;
  }

  activeSearchQuery = query;
  searchIsGlobal = global;
  if (global) {
    // No category/shelf filter for a global search -- clear whatever was
    // current so loadPage() doesn't intersect it with one.
    currentCategory = -1;
    currentBookshelfId = 0;
    currentBookshelfName.clear();
  }
  state = BrowserState::BROWSING;
  selectorIndex = 0;
  topIndex = 0;
  showLoadingBeforeFetch();
  loadPage(1);
}

void BookFusionBrowserActivity::loadPage(int pageIndex) {
  if (pageIndex < 1) return;  // 1-indexed -- see BookFusionSyncClient::searchBooks().
  showLoadingBeforeFetch();

  // Release right before the real request, matching BookFusionAuthActivity/
  // BookFusionSyncActivity: screens rendered on the way here (Wi-Fi selection,
  // category selection, this loading screen) can lazily reload the SD font,
  // so releasing any earlier than this doesn't reliably free memory for TLS.
  sdFontSystem.releaseForNetwork(renderer);

  // The previous page's books are dead weight once we've decided to
  // navigate; free them before the fetch instead of after, so they aren't
  // sitting in heap alongside the TLS handshake below.
  page = BookFusionSearchResult{};

  // The e-ink framebuffer(s) are a permanent multi-KB heap resident that caps
  // the largest contiguous allocatable block well below what wolfSSL needs
  // for a TLS handshake+read (confirmed via serial log: MEMORY_E mid-read
  // despite tens of KB of nominally free heap). Free them for the duration
  // of the request — no display operations may happen until the realloc
  // below — and bring them back before the next render.
  renderer.releaseFrameBuffersForNetwork();

  BookFusionSearchResult result;
  // A global search (see performSearch()) has no category/shelf of its own --
  // currentCategory is -1 in that case, so CATEGORIES[] must not be indexed.
  const bool globalSearch = !activeSearchQuery.empty() && searchIsGlobal;
  const char* listParam = globalSearch                     ? nullptr
                          : currentBookshelfId != 0 ? nullptr
                                                     : CATEGORIES[currentCategory].list;
  const uint32_t bookshelfIdParam = globalSearch ? 0 : currentBookshelfId;
  // A user-chosen sort applies regardless of category vs. shelf browsing -- unlike the
  // fixed per-category defaults below, which only ever applied to the fixed categories
  // and left shelf browsing on the server's own default ("added_at-desc").
  std::string userSortParam;
  const char* sortParam;
  if (SETTINGS.bookFusionSortField < BOOKFUSION_SORT_FIELD_COUNT) {
    static constexpr const char* kFieldApiNames[BOOKFUSION_SORT_FIELD_COUNT] = {"added_at", "last_read_at", "author",
                                                                                "title"};
    userSortParam = std::string(kFieldApiNames[SETTINGS.bookFusionSortField]) +
                    (SETTINGS.bookFusionSortAscending ? "-asc" : "-desc");
    sortParam = userSortParam.c_str();
  } else if (globalSearch) {
    sortParam = nullptr;  // server default (added_at-desc)
  } else {
    sortParam = currentBookshelfId != 0 ? nullptr : CATEGORIES[currentCategory].sort;
  }
  const char* queryParam = activeSearchQuery.empty() ? nullptr : activeSearchQuery.c_str();
  const auto err =
      BookFusionSyncClient::searchBooks(pageIndex, listParam, result, bookshelfIdParam, sortParam, queryParam);

  if (!renderer.reallocFrameBuffersAfterNetwork()) {
    LOG_ERR("BFBrowser", "Framebuffer realloc failed after network fetch");
    ESP.restart();
  }

  if (err != BookFusionSyncClient::OK) {
    state = BrowserState::ERROR;
    errorMessage = BookFusionSyncClient::errorString(err);
    requestUpdate();
    return;
  }

  page = std::move(result);
  selectorIndex = 0;
  topIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
}

void BookFusionBrowserActivity::downloadBook(const BookFusionBook& book) {
  // The row is already disabled for non-EPUB formats, but that only blocks
  // touch activation -- the physical Confirm button reaches this function
  // directly regardless of row state, so refuse here too rather than burn
  // bandwidth on a file the device can't open.
  if (!bookFusionFormatIsEpub(book)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_BF_FORMAT_UNSUPPORTED);
    requestUpdate();
    return;
  }

  state = BrowserState::DOWNLOADING;
  downloadTitle = book.title;
  downloadAuthor = book.author;
  statusMessage = tr(STR_CONNECTING);
  downloadProgress = 0;
  // From the search API response, not the live transfer -- lets the
  // filesize/estimate line show immediately (matches InsiderPhD's fork).
  // The transfer's own progress callback overwrites this with the actual
  // measured total once it starts, which is authoritative if it differs.
  downloadTotal = book.downloadSize;
  goHomeAfterCancel = false;

  // Must actually wait for this render (not just requestUpdate(true), which
  // returns before the e-ink refresh finishes): the framebuffer release just
  // below frees the buffer this refresh may still be reading from mid-flight,
  // which crashed with a null-framebuffer store fault when this used the
  // non-waiting form.
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFBrowser", "Downloading screen could not be rendered before fetch");
    requestUpdate(true);
  }

  // Same reasoning as loadPage(): the "Downloading" status screen just
  // rendered above and can lazily reload the SD font, so release right
  // before the real request instead of any earlier.
  sdFontSystem.releaseForNetwork(renderer);

  // Same framebuffer-release reasoning as loadPage(). Scoped to just this
  // quick metadata call — the EPUB transfer below renders progress updates
  // throughout, which needs the framebuffer, so it can't be wrapped the
  // same way.
  renderer.releaseFrameBuffersForNetwork();

  std::string downloadUrl;
  const auto urlErr = BookFusionSyncClient::getDownloadUrl(book.bookId, downloadUrl);

  if (!renderer.reallocFrameBuffersAfterNetwork()) {
    LOG_ERR("BFBrowser", "Framebuffer realloc failed after network fetch");
    ESP.restart();
  }

  // End the browse session now: the actual EPUB transfer below goes through
  // HttpDownloader on its own connection, so there's no reason to keep the
  // BookFusion session's idle TLS connection open during a potentially long
  // download.
  BookFusionSyncClient::endSession();
  if (urlErr != BookFusionSyncClient::OK) {
    state = BrowserState::ERROR;
    errorMessage = BookFusionSyncClient::errorString(urlErr);
    requestUpdate();
    return;
  }

  const char* downloadFolder = SETTINGS.bookFusionDownloadFolder;
  const bool useDownloadFolder = downloadFolder[0] != '\0';
  if (useDownloadFolder && !Storage.exists(downloadFolder) && !Storage.mkdir(downloadFolder)) {
    LOG_ERR("BFBrowser", "Could not create download folder %s", downloadFolder);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  const std::string filename = resolveBookFilePath(book);
  LOG_DBG("BFBrowser", "Downloading book %lu -> %s", (unsigned long)book.bookId, filename.c_str());

  // Clear any stale cache from a previous file at this exact path before
  // writing anything new into it.
  clearBookCache(filename);

  // No cover fetch here (or anywhere else in BookFusion): the JPEG/PNG
  // decoder needs a single large contiguous block (~53KB) that's chronically
  // unavailable at this point in the flow (WiFi + a searchBooks() TLS
  // round-trip have already fragmented the heap), consistently failing on
  // hardware and taking the framebuffer's own post-fetch reallocation down
  // with it -- confirmed via a reproducible restart loop. The download
  // screen simply renders without a cover.

  statusMessage = tr(STR_DOWNLOAD_WAIT);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFBrowser", "Downloading screen could not be rendered before transfer");
    requestUpdate(true);
  }

  // Release again for the transfer itself: it's the same wolfSSL heap
  // pressure as the quick metadata calls above, just spread over a much
  // longer window. The progress callback below reallocates/renders/releases
  // around each throttled update instead of holding the buffer for the
  // whole download.
  renderer.releaseFrameBuffersForNetwork();

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested) return true;
    mappedInput.update();
    if (mappedInput.wasHomeGesture()) {
      goHomeAfterCancel = true;
      cancelRequested = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested = true;
    }
    return cancelRequested;
  };

  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = pollCancel;
  downloadOptions.bufferSize = DOWNLOAD_BUFFER_SIZE;
  downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  downloadOptions.preservePartial = true;
  downloadOptions.resumePartial = true;
  // Defense-in-depth only: the URL from getDownloadUrl() is expected to be
  // pre-signed, and this header is only ever sent same-origin with it (see
  // HttpDownloader's redirect gating), so it's a no-op if the URL doesn't need it.
  downloadOptions.bearerToken = BookFusionSyncClient::getBearerToken();
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        mappedInput.update();
        if (uiReady) {
          const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
          if (snap.touchPressed || snap.touchReleased) app.route(snap);
        }
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;

          // The buffer is released for the whole transfer (see above); bring
          // it back just long enough to draw this update, then hand it back.
          if (!renderer.reallocFrameBuffersAfterNetwork()) {
            LOG_ERR("BFBrowser", "Framebuffer realloc failed during download progress");
            ESP.restart();
          }
          if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
            LOG_ERR("BFBrowser", "Download progress screen could not be rendered");
            requestUpdate(true);
          }
          renderer.releaseFrameBuffersForNetwork();
        }
      },
      &cancelRequested, "", "", downloadOptions);

  // The buffer is left released by the loop above (every progress render
  // re-releases it after drawing) regardless of how the transfer ended;
  // bring it back before any of the result-handling below renders.
  if (!renderer.reallocFrameBuffersAfterNetwork()) {
    LOG_ERR("BFBrowser", "Framebuffer realloc failed after download");
    ESP.restart();
  }

  if (result == HttpDownloader::OK) {
    // Cover was already fetched into this same cache path before the
    // transfer started (see above) -- nothing left to do here for it.
    BookFusionBookIdStore::saveBookId(filename, book.bookId);
    state = BrowserState::DOWNLOAD_COMPLETE;
  } else if (result == HttpDownloader::ABORTED) {
    LOG_INF("BFBrowser", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    mappedInput.suppressNextBackRelease();
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void BookFusionBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    loadShelvesAndShowMenu();
    return;
  }
  launchWifiSelection();
}

void BookFusionBrowserActivity::loadShelvesAndShowMenu() {
  showLoadingBeforeFetch();

  sdFontSystem.releaseForNetwork(renderer);
  renderer.releaseFrameBuffersForNetwork();

  bookshelves = BookFusionBookshelfList{};
  const auto err = BookFusionSyncClient::searchBookshelves(bookshelves);
  // Best-effort: a failed fetch just means the menu shows categories only,
  // same as if the user has no shelves at all -- not worth an error screen.
  bookshelvesLoaded = err == BookFusionSyncClient::OK;
  if (!bookshelvesLoaded) {
    LOG_ERR("BFBrowser", "searchBookshelves failed: %s", BookFusionSyncClient::errorString(err).c_str());
  }

  if (!renderer.reallocFrameBuffersAfterNetwork()) {
    LOG_ERR("BFBrowser", "Framebuffer realloc failed after network fetch");
    ESP.restart();
  }

  state = BrowserState::CATEGORY_SELECTION;
  requestUpdate();
}

void BookFusionBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    loadShelvesAndShowMenu();
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
