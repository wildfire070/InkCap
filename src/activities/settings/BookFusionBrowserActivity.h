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

  // Sort popup (BROWSING screen only): touch via the header's trailing "Sort" button
  // (ACTION_SORT/onSortEvent), non-touch via long-press Confirm (see loop()).
  SortPopup sortPopup;
  bool longPressConfirmHandled = false;
  void openSortPopup();

  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPageButtonEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSortEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen, const char* title, bool showSort = false);
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
