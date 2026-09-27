#pragma once

#include <string>
#include <vector>

#include "../../Ao3LibraryMetadata.h"
#include "../../Ao3SortFilterState.h"
#include "../../Ao3ViewEntry.h"
#include "../../BookStatus.h"
#include "../../util/ButtonNavigator.h"
#include "../Activity.h"
#include "Ao3LibrarySettingsActivity.h"

// Rating / completion filter criteria (see passesFilter).
struct FilterHashes {
  bool ratingActive;
  char ratingValue;
  bool completionActive;
  bool completionValue;
};

inline FilterHashes computeFilterHashes(const SortFilterState& state) {
  FilterHashes h;
  h.ratingActive = state.rating != 0;
  h.ratingValue = state.rating;
  h.completionActive = state.completion != -1;
  h.completionValue = state.completion == 1;
  return h;
}

class Ao3LibraryActivity final : public Activity {
 public:
  explicit Ao3LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t initialSelectorIndex = 0)
      : Activity("Ao3Library", renderer, mappedInput), initialSelectorIndex_(initialSelectorIndex) {}

  void onEnter() override;
  void loop() override;
  void onExit() override;
  void render(RenderLock&&) override;
  static bool pendingTransferScan;
  // Flags a rescan for the next time the library opens. Also persists a marker file:
  // every Wi-Fi transfer ends in a silent restart, which would otherwise wipe the
  // in-RAM flag before the library is ever opened.
  static void requestTransferScan();

 private:
  enum class IndexState {
    UNKNOWN,  // not yet loaded
    OK,       // index file valid
    MISSING,  // ao3_library_index.bin does not exist
    CORRUPT   // file exists but failed magic/version/OOM sanity check
  };

  enum class ScreenState { LIBRARY, FILTER_PANEL, MANAGE_PANEL };

  std::vector<ViewEntry> viewEntries;
  size_t selectorIndex = 0;
  IndexState indexState = IndexState::UNKNOWN;
  ScreenState screenState = ScreenState::LIBRARY;
  ButtonNavigator buttonNavigator;
  bool swapNavButtons = false;

  // Page cache
  Ao3LibraryMetadata pageCache[3];
  BookStatus pageCacheStatus[3] = {BookStatus::START, BookStatus::START, BookStatus::START};
  // 0-based Marked for Later queue position for the slot's fic, or -1 if not
  // marked. See getMarkedPosition().
  int pageCacheMarkedPosition[3] = {-1, -1, -1};
  // True for a slot whose scraped info (ao3_library_info) could not be loaded: the row falls back to what the index
  // and the marked-for-later store know and shows '?' in the details square.
  bool pageCacheInfoMissing[3] = {false, false, false};
  // True for a slot whose fic is marked for later but has no AO3 index record at all (see UnindexedMarked): only its
  // title/author/path are known, and its rating and completion are unknown.
  bool pageCacheUnindexed[3] = {false, false, false};
  std::vector<std::string> wrappedSummary[3];
  int cachedPage = -1;
  bool buttonsSetup = false;

  // Filter & Sort State
  SortFilterState activeState;
  SortFilterState pendingState;
  std::string ao3Folder;
  int overlayRowIndex = 0;      // 0=Rating, 1=Completion, 2=Sort By, 3=Order, 4=Show, 5=Confirm
  int managePanelRowIndex = 0;  // 0=Index New Books, 1=AO3 Library Settings

  // Cache hashes of the active Marked for Later / New Chapters view, in the store's own order.
  // Empty for the other views. Both stores are capped at 10, so linear lookups are fine.
  std::vector<uint64_t> viewOrder_;

  // A Marked for Later fic that is not in the AO3 index (marked from the File Browser, never indexed, or its
  // index record was dropped). The Marked view is built from the index, so these are added on top from the
  // store's own title/author/path so no marked fic is ever missing from the view. At most 10 (the store cap).
  struct UnindexedMarked {
    uint64_t cacheHash;
    std::string path;
    std::string title;
    std::string author;
  };
  std::vector<UnindexedMarked> unindexedMarked_;
  const UnindexedMarked* findUnindexedMarked(uint64_t cacheHash) const;

  size_t initialSelectorIndex_ = 0;
  bool skipNextBackRelease = false;
  bool autoIndexOnOpen_ = false;
  bool receivedReviewLaunched_ = false;
  bool autoIndexLaunched_ = false;

  void loadViewEntries();
  void loadPageCache(int page);
  BookStatus getBookStatus(uint64_t cacheHash);
  int getMarkedPosition(uint64_t cacheHash);

  void renderEntry(RenderLock& lock, int y, const ViewEntry& ve, int cacheSlot, bool selected);
  void drawAo3Square(RenderLock& lock, int x, int y, int s, char rating, bool detailsMissing, bool completed,
                     BookStatus status, int markedPosition = -1, bool completionUnknown = false);

  void renderSymbol(int x, int y, int s, char c, bool tl, bool tr, bool bl, bool br, int yOffset = 0);
  void renderStatusSymbol(int x, int y, int s, BookStatus status, bool tl, bool tr, bool bl, bool br,
                          int yOffset = 0, int markedPosition = -1);
  void renderDetailsSymbol(int x, int y, int s, bool missing, bool tl, bool tr, bool bl, bool br, int yOffset = 0);
  void renderCompletionSymbol(int x, int y, int s, bool completed, bool tl, bool tr, bool bl, bool br, int yOffset = 0);

  // Sorting & Filtering helpers
  void loadSettings();
  void loadSortFilterState();
  void saveSortFilterState() const;
  void resortViewEntries();
  void rebuildViewEntries();
  void addUnindexedMarked(const std::vector<uint8_t>& markedInIndex, const FilterHashes& filters, bool hideFinished);
  void applyStateChange(const SortFilterState& prev, const SortFilterState& next);
  bool passesFilter(const ViewEntry& v, const FilterHashes& h) const;
  bool isStoreView() const {
    return activeState.view == LibraryView::MARKED_FOR_LATER || activeState.view == LibraryView::NEW_CHAPTERS;
  }
  void loadViewOrder();
  int viewPosition(uint64_t cacheHash) const;

  // Rendering subsets
  void renderLibrary(RenderLock& lock);
  void renderFilterOverlay();
  void renderManagePanel();
};
