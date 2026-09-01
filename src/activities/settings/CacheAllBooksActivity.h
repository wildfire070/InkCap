#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

struct Rect;

// Settings action (System > Files & Cache > Cache All Books): recursively walks the
// whole SD card and builds the BookMetadataCache for any EPUB that doesn't have one
// yet -- the same lazy build FileBrowserActivity's sort feature does per-folder on
// first use (see ensureSortCache()), just run once up front for the entire card so
// sort (and anything else that reads the cache) works everywhere immediately, not
// just for books that happen to get opened first. Re-running after adding new books
// only pays the cost for the new ones -- already-cached books are skipped near-instantly.
class CacheAllBooksActivity final : public Activity {
 public:
  explicit CacheAllBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CacheAllBooks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

  // Persisted exclusion list, own JSON file (matches Ao3LibrarySettingsActivity's
  // own ao3_settings.json for its own, separate excludedFolders list) -- shared
  // between this activity (which reads it to skip folders while scanning) and
  // SettingsActivity's "Cache Exclusions" row (which edits it via the same
  // Ao3FolderPickerActivity MULTI picker AO3 library exclusions already use).
  static std::vector<std::string> loadExclusions();
  static void saveExclusions(const std::vector<std::string>& folders);

 private:
  enum State { WARNING, CACHING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int cachedCount = 0;
  int failedCount = 0;
  bool lowHeapAborted = false;
  // Leaf filenames only (not full paths) for the SUCCESS screen's failure list,
  // capped so a pathological run can't grow this unbounded -- failedCount is the
  // real total; the list is best-effort detail, with "+N more" past the cap.
  static constexpr size_t kMaxFailedNamesShown = 20;
  std::vector<std::string> failedNames;

  std::vector<std::string> excludedFolders;
  bool isExcluded(const std::string& path) const;

  // Two passes: counting first (cheap directory listings only, no cache building)
  // gives buildCachesRecursive() a real total to show progress against, without
  // holding every book's path in memory at once for a potentially large library --
  // this device only has ~320KB of DRAM total. total/processed/popupRect are
  // threaded through as parameters rather than stored on the activity, matching
  // how RecentBooksGridActivity's own build-cache-then-progress loop keeps them local.
  int countEpubsRecursive(const std::string& dirPath);
  void buildCachesRecursive(const std::string& dirPath, int total, int& processed, bool& showingPopup,
                            Rect& popupRect);
  void cacheAllBooks();
  void startCaching();
};
