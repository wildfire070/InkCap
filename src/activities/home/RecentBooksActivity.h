#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "Ao3MarkedForLaterStore.h"
#include "Ao3NewChaptersStore.h"
#include "Ao3WipsStore.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  // FreeInkApp hosts the book list (themed rows, icons, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  // Order matches the on-screen tab bar, left to right. RecentBooks is last
  // (and the default) so the screen's original single-list behavior is what
  // a fresh launch shows, unchanged from before the Dashboard existed.
  enum class DashboardTab : uint8_t { MarkedForLater, NewChapters, Wips, RecentBooks };
  static constexpr int TAB_COUNT = 4;
  DashboardTab activeTab = DashboardTab::RecentBooks;

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;
  bool pendingCacheDeletedFeedback = false;
  unsigned long cacheDeletedFeedbackShowTime = 0UL;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  // Dashboard tab state (tabs 0-2). Each store's own entry type is kept
  // as-is rather than projected into a shared struct -- only three call
  // sites (row count, row build, row tap) need to branch on activeTab.
  std::vector<Ao3MarkedForLaterEntry> markedForLaterEntries;
  std::vector<Ao3NewChaptersEntry> newChaptersEntries;
  std::vector<Ao3WipEntry> wipsEntries;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTabEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  // Data loading
  void loadRecentBooks();
  void loadActiveTabEntries();
  void reloadAfterBookAction();

  // Number of rows in the currently active tab (whichever vector it reads from).
  int activeTabCount() const;
  // (path, title, author) for row `index` of the currently active tab.
  struct DashboardRow {
    std::string path;
    std::string title;
    std::string author;
  };
  DashboardRow activeTabRow(size_t index) const;

  void promptDeleteBook(const std::string& path, const std::string& title);
  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);
  void showBookActionMenu(size_t bookIndex, bool ignoreInitialConfirmRelease = false);
  // Opens Book Info for recentBooks[bookIndex], wiring Left/Right = Previous/Next
  // Book against recentBooks itself (not reloaded between hops, so indices stay
  // stable while the user pages through -- reloadAfterBookAction() only runs on
  // the final exit, once BookDetailsActivity finishes without a nav result).
  void openBookDetails(size_t bookIndex);

  // Generic long-press action menu for the three Dashboard tabs (Marked for
  // Later, New Chapters, WIPs). Unlike showBookActionMenu() this has no
  // book-index-based Book Info Prev/Next chaining or Remove-from-Recents --
  // reuses BookActions::buildBookActionItems(), which already self-gates
  // Pin/Mark-for-Later based on actual store membership regardless of which
  // tab a path was reached from.
  void showDashboardEntryActionMenu(const std::string& path, const std::string& title, const std::string& author,
                                    bool ignoreInitialConfirmRelease = false);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
