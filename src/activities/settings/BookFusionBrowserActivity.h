#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"
#include "components/SortPopup.h"
#include "util/ButtonNavigator.h"

/**
 * BookFusion library browser: pick a category, then page through books in
 * it, downloading EPUBs with cancel/resume support. Modeled closely on
 * OpdsBookBrowserActivity's state machine and download handling, adapted for
 * BookFusion's flat category+page API instead of OPDS feed navigation.
 */
class BookFusionBrowserActivity final : public Activity {
 public:
  // Server-side sort fields for the BROWSING screen, offered via SortPopup. Order
  // must match kFieldApiNames in loadPage().
  enum class SortField : uint8_t { Date = 0, LastRead = 1, Author = 2, Title = 3 };
  static constexpr int BOOKFUSION_SORT_FIELD_COUNT = 4;

  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    CATEGORY_SELECTION,
    BROWSING,
    DOWNLOADING,
    DOWNLOAD_COMPLETE,
    ERROR
  };

  explicit BookFusionBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  using UiApp = freeink::ui::FreeInkApp<24, 4>;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;

  int selectedCategory = 0;  // Highlighted row on the category screen: [0, totalMenuRows()).
  int currentCategory = 0;   // Fixed category currently being browsed (meaningless if currentBookshelfId != 0).

  BookFusionBookshelfList bookshelves;
  bool bookshelvesLoaded = false;  // Fetch is best-effort; false just means the menu shows categories only.
  uint32_t currentBookshelfId = 0;  // 0 = browsing a fixed category, not a shelf.
  std::string currentBookshelfName;

  BookFusionSearchResult page;  // Current page of books (vector capacity reserved to BOOKFUSION_BOOKS_PER_PAGE).
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;

  // Static download-card fields (title/author/cover set once when the
  // download starts; filesize/estimate derived from downloadTotal once
  // known) -- matches InsiderPhD's fork's frozen info-card layout instead
  // of a live progress bar. See buildDownloadScreen().
  std::string downloadTitle;
  std::string downloadAuthor;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool goHomeAfterCancel = false;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  // Sort popup (BROWSING screen only): the persistent edge tab (touch tap
  // target, and a visible hint for non-touch devices -- see
  // FileBrowserActivity's own tab for the same rationale), non-touch trigger
  // is PageBack/PageForward -- same as File Browser, for a consistent gesture
  // across both screens (see loop()). That freed page-turn (PageBack/
  // PageForward's old job here) onto a long-press of Left/Right instead, since
  // those were the only buttons left; longPressPageTurnHandled swallows the
  // row-scroll release that follows a hold that already jumped a page.
  SortPopup sortPopup;
  bool longPressPageTurnHandled = false;
  Rect sortButtonRect{0, 0, 0, 0};
  // PageForward shares its physical button with the Power+Down screenshot
  // combo (see ButtonShortcutController::updatePowerDown); the two presses
  // rarely land in the same debounce window, so a screenshot attempt's Down
  // edge can register -- and open Sort -- a frame or two before Power does.
  // 0 = no pending PageForward action; otherwise the millis() timestamp
  // PageForward was pressed. While pending: a Power press/screenshot-chord
  // flag cancels it outright (see updatePageForwardSortAndSearch()); holding
  // past SEARCH_LONG_PRESS_MS opens Search instead of Sort; releasing before
  // that opens Sort. Sort firing on release (not while held, as it used to)
  // is what makes room for the long-press-Search escalation -- there's no way
  // to know a hold is "long" until it either keeps going or ends.
  unsigned long pendingSortFromPageForwardMs = 0;
  // True once this pending press has already opened Search (long-press),
  // so the release that follows doesn't also open Sort.
  bool searchLongPressHandled = false;
  void openSortPopup();

  // Search (BROWSING and CATEGORY_SELECTION screens): a free-text query sent
  // to BookFusion's own /api/user/books/search "query" field -- confirmed via
  // BookFusion's official KOReader plugin (bf_browser.lua), which sends this
  // alongside list/bookshelf_id for an in-list search, or alone for a global
  // one from the top-level categories screen. Touch entry point is a second
  // edge tab below Sort's (Search tab alone on CATEGORY_SELECTION, since Sort
  // doesn't apply there); non-touch is a long-press on PageForward -- the
  // same button as Sort's short-press trigger, via
  // updatePageForwardSortAndSearch() (see pendingSortFromPageForwardMs).
  std::string activeSearchQuery;  // empty = no search filter active
  // True when the active search has no category/shelf filter (launched from
  // CATEGORY_SELECTION); false when it's scoped to whatever category/shelf
  // was open when Search was launched (launched from BROWSING).
  bool searchIsGlobal = false;
  Rect searchButtonRect{0, 0, 0, 0};
  // Runs PageForward's shared Sort(short-press)/Search(long-press) state
  // machine for one loop() call; returns true only when it actually opened
  // Sort or Search this frame, in which case the caller should return
  // immediately, same as every other input branch here. A screenshot-combo
  // cancellation returns false (nothing happened -- unlike Sort/Search, it
  // isn't an action the rest of loop() needs to skip a frame for).
  // `allowSort` is false on CATEGORY_SELECTION, where Sort doesn't apply and
  // PageForward's short-press does nothing.
  bool updatePageForwardSortAndSearch(bool allowSort, bool launchesGlobalSearch);
  void launchSearch(bool global);
  void performSearch(const std::string& query, bool global);

  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPageButtonEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen, const char* title);
  void buildCategoryScreen(UiApp::ScreenType& screen);
  void buildBrowsingScreen(UiApp::ScreenType& screen);
  void buildDownloadScreen(UiApp::ScreenType& screen);
  void buildDownloadCompleteScreen(UiApp::ScreenType& screen);
  void buildStatusScreen(UiApp::ScreenType& screen);

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void loadShelvesAndShowMenu();
  void loadPage(int pageIndex);
  void selectCategory(int index);
  void downloadBook(const BookFusionBook& book);
  void activateSelected();
  int totalMenuRows() const;
};
