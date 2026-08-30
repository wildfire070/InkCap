#pragma once
#include <string>
#include <vector>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"

/**
 * Bulk-refreshes BookFusion-sourced cover art and reading position for
 * every local book already linked to a BookFusion book ID (i.e. every book
 * previously downloaded through BookFusionBrowserActivity).
 *
 * There is no "get book by ID" endpoint, so this walks the same paginated
 * /api/user/books/search all-books listing BookFusionBrowserActivity uses,
 * matching each returned book against the local set by ID -- the same
 * approach that activity already relies on for browsing.
 *
 * Never overwrites a book's reading position if progress.bin already
 * exists (i.e. the user has actually opened it locally) -- refresh only
 * fills in a position for a downloaded-but-never-opened book.
 */
class RefreshBookFusionMetadataActivity final : public Activity {
 public:
  explicit RefreshBookFusionMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RefreshBFMeta", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == RUNNING; }

 private:
  enum State { WARNING, WIFI_SELECTION, CONNECTING, RUNNING, SUCCESS, ERROR };
  State state = WARNING;

  std::string statusMessage;
  std::string errorMessage;

  std::vector<std::string> localBookPaths;  // Books with a BookFusion sidecar, not yet matched.
  int totalLocal = 0;
  int matched = 0;
  int coversOk = 0;
  int positionsOk = 0;
  int failed = 0;

  void onWifiSelectionComplete(bool connected);
  void runRefresh();
  void refreshOneBook(const BookFusionBook& book, const std::string& localPath);
  void returnToCaller();
};
