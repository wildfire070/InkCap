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
constexpr fui::ActionId ACTION_SORT = 4;
// Long-press threshold for the non-touch Sort entry point (Confirm's short-press
// already downloads the highlighted book on this screen).
constexpr unsigned long SORT_LONG_PRESS_MS = 600;
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
  app.on(ACTION_SORT, &BookFusionBrowserActivity::onSortEvent, this);
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

void BookFusionBrowserActivity::onSortEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<BookFusionBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->openSortPopup();
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

    if (!mappedInput.hasTouchHardware() && !longPressConfirmHandled &&
        mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= SORT_LONG_PRESS_MS) {
      longPressConfirmHandled = true;
      openSortPopup();
      return;
    }

    if (bookCount > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (longPressConfirmHandled) {
        longPressConfirmHandled = false;
        return;
      }
      activateSelected();
      return;
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

    // Dedicated page-turn buttons jump pages directly in one press, without
    // needing to scroll to a row -- Previous/Next Page live in a fixed
    // footer instead of the scrollable list (see buildBrowsingScreen()),
    // which is also touch-tappable there via ACTION_PREV_PAGE/NEXT_PAGE.
    if (page.currentPage > 1 && mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      loadPage(page.currentPage - 1);
      return;
    }
    if (page.hasMore && mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      loadPage(page.currentPage + 1);
      return;
    }

    // Up/Down stay bounded to the current page's books -- no falling off
    // either end into the adjacent page, since that's what the dedicated
    // page-turn buttons above are for.
    if (bookCount > 0) {
      const auto moveSelection = [this, bookCount](const int index) {
        selectorIndex = index;
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, bookCount);
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, &moveSelection, bookCount] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, bookCount)); });
      buttonNavigator.onPreviousRelease([this, &moveSelection, bookCount] {
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

void BookFusionBrowserActivity::screenHeader(UiApp::ScreenType& screen, const char* title, const bool showSort) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  fui::HeaderProps header;
  header.title = title;
  header.borderEdges = fui::EdgeBottom;
  if (showSort) {
    // Same trailingLabel/trailingAction mechanism OpdsBookBrowserActivity's header
    // search button already uses -- touch dispatches through ACTION_SORT below; a
    // non-touch entry point (long-press Confirm) is handled separately in loop().
    header.trailingLabel = tr(STR_SORT);
    header.trailingAction = ACTION_SORT;
  }
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
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  screen.list(props);
}

void BookFusionBrowserActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, currentBookshelfId != 0 ? currentBookshelfName.c_str() : I18N.get(CATEGORIES[currentCategory].nameId),
              /*showSort=*/true);

  if (page.books.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Footer strip: Previous Page (left) / "N / M" indicator (center) / Next
  // Page (right), reserved from the bottom before the list claims the rest
  // of the body. Previous/Next live here rather than as rows inside the
  // scrollable list -- with 10 books per page they no longer fit alongside
  // the books without scrolling, and putting them in a fixed footer means
  // they're always reachable in one tap/button press regardless of scroll
  // position (see loop()'s PageBack/PageForward handling for the button
  // path). theme().smallText already carries the FONT_SMALL slot (a slot
  // index, not a raw font ID -- see the constructor's slot rebind).
  const auto& theme = screen.theme();
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
  const auto rows = configureUiList(props, theme, screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(page.books.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);

  if (page.currentPage > 1) {
    fui::ButtonProps prevBtn;
    prevBtn.label = tr(STR_PREV_PAGE);
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

  char indicator[24];
  if (page.totalCount > 0) {
    const int totalPages = totalPagesFor(page);
    snprintf(indicator, sizeof(indicator), "%d / %d", page.currentPage, totalPages);
  } else if (page.hasMore) {
    snprintf(indicator, sizeof(indicator), "%d / %d+", page.currentPage, page.currentPage);
  } else {
    snprintf(indicator, sizeof(indicator), "%d / %d", page.currentPage, page.currentPage);
  }
  const fui::Rect indicatorRect{static_cast<int16_t>(footerRect.x + navBtnW), footerRect.y,
                                static_cast<int16_t>(footerRect.width - navBtnW * 2), footerRect.height};
  screen.target().text(indicatorRect, indicator, indicatorStyle);
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
  // tr(x) is a macro expanding to I18N.get(StrId::x) via token-pasting -- it can't take
  // a runtime variable, so this array/loop calls I18N.get() directly instead.
  static const StrId kFieldLabelIds[BOOKFUSION_SORT_FIELD_COUNT] = {
      StrId::STR_SORT_DATE, StrId::STR_SORT_LAST_READ, StrId::STR_SORT_AUTHOR, StrId::STR_TITLE};
  std::vector<std::string> labels;
  labels.reserve(BOOKFUSION_SORT_FIELD_COUNT);
  for (const StrId id : kFieldLabelIds) labels.emplace_back(I18N.get(id));

  const uint8_t fieldRaw = SETTINGS.bookFusionSortField;
  const int activeField = fieldRaw < BOOKFUSION_SORT_FIELD_COUNT ? static_cast<int>(fieldRaw) : -1;
  const bool ascending = SETTINGS.bookFusionSortAscending != 0;

  sortPopup.show(
      StrId::STR_SORT, std::move(labels), activeField, ascending,
      [this](int field, bool asc) {
        SETTINGS.bookFusionSortField = static_cast<uint8_t>(field);
        SETTINGS.bookFusionSortAscending = asc ? 1 : 0;
        SETTINGS.saveToFile();
        selectorIndex = 0;
        topIndex = 0;
        showLoadingBeforeFetch();
        loadPage(1);
      },
      [this] { requestUpdate(); });
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
  const char* listParam = currentBookshelfId != 0 ? nullptr : CATEGORIES[currentCategory].list;
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
  } else {
    sortParam = currentBookshelfId != 0 ? nullptr : CATEGORIES[currentCategory].sort;
  }
  const auto err = BookFusionSyncClient::searchBooks(pageIndex, listParam, result, currentBookshelfId, sortParam);

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
