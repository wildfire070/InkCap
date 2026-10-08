#include "Ao3LibraryActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <new>

#include "../../Ao3Librarian.h"
#include "../../Ao3MarkedForLaterStore.h"
#include "../../Ao3NewChaptersStore.h"
#include "../../Ao3StoreMaintenance.h"
#include "../../CrossPointState.h"
#include "../../MappedInputManager.h"
#include "../../RecentBooksStore.h"
#include "../../components/TouchHeaderBackButton.h"
#include "../../components/TouchRegistry.h"
#include "../../components/UITheme.h"
#include "../../fontIds.h"
#include "../../util/Ao3ReceiveUtils.h"
#include "Ao3IndexActivity.h"
#include "Ao3LibrarySettingsActivity.h"
#include "Ao3ReceivedReviewActivity.h"
#include "BookActionActivity.h"
#include "CrossPointSettings.h"

// ---------------------------------------------------------------------------
//  onEnter
// ---------------------------------------------------------------------------

bool Ao3LibraryActivity::pendingTransferScan = false;

namespace {
constexpr char PENDING_SCAN_MARKER[] = "/.crosspoint/pending_ao3_scan";

// Sort & Filter overlay geometry, shared by renderFilterOverlay() and the tap-outside-to-close check.
constexpr int OVERLAY_START_Y = 48;
constexpr int OVERLAY_HEIGHT = 340;
constexpr int OVERLAY_ROW_SHOW = 4;
constexpr int OVERLAY_ROW_CONFIRM = 5;
constexpr int OVERLAY_ROW_COUNT = 6;

// The Manage Panel's triangle indicator (in the button-hints bar) sits close enough to the right edge
// that the X4 Pro's recessed bezel visually clips it -- same class of issue UIThemeTokens.h's own
// listScrollInset already works around for the scroll indicator. Push it further inward on that board
// specifically; every other board keeps the original position.
int managePanelTriangleX(const int screenWidth) {
#ifndef SIMULATOR
  if (BoardConfig::isX4Pro()) return screenWidth - 36;
#endif
  return screenWidth - 26;
}

const char* viewLabel(LibraryView v) {
  switch (v) {
    case LibraryView::MARKED_FOR_LATER:
      return "Marked for Later";
    case LibraryView::NEW_CHAPTERS:
      return "New Chapters";
    case LibraryView::WIPS:
      return "WIPs";
    default:
      return "All";
  }
}
}

void Ao3LibraryActivity::requestTransferScan() {
  pendingTransferScan = true;
  Storage.writeFile(PENDING_SCAN_MARKER, "");
}

void Ao3LibraryActivity::onEnter() {
  if (Storage.exists(PENDING_SCAN_MARKER)) {
    Storage.remove(PENDING_SCAN_MARKER);
    pendingTransferScan = true;
  }
  buttonNavigator.setMappedInputManager(mappedInput);
  indexState = IndexState::UNKNOWN;
  screenState = ScreenState::LIBRARY;
  selectorIndex = initialSelectorIndex_;  // restore position when returning from reader

  // If the user arrived here by holding BACK in the reader,
  // the button is still physically pressed. We must ignore the subsequent release.
  skipNextBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  autoIndexLaunched_ = false;
  receivedReviewLaunched_ = false;
  selfHealAo3PathStores();
  loadSettings();
  loadSortFilterState();
  requestUpdate();
}

void Ao3LibraryActivity::loadSettings() {
  ao3Folder = "";
  autoIndexOnOpen_ = false;

  const char* path = "/.crosspoint/ao3_settings.json";
  if (!Storage.exists(path)) return;

  String json = Storage.readFile(path);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  ao3Folder = doc["ao3Folder"] | "";
  autoIndexOnOpen_ = doc["autoIndexOnOpen"] | false;
}

void Ao3LibraryActivity::onExit() { Activity::onExit(); }

// ---------------------------------------------------------------------------
//  loadViewEntries — delegates to rebuildViewEntries
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loadViewEntries() {
  if (indexState != IndexState::UNKNOWN) return;
  rebuildViewEntries();
}

// ---------------------------------------------------------------------------
//  getBookStatus — reads the ao3-status.bin sidecar for the given cache hash
// ---------------------------------------------------------------------------

BookStatus Ao3LibraryActivity::getBookStatus(uint64_t cacheHash) {
  return Ao3Librarian::getBookStatus("/.crosspoint/epub_" + std::to_string(cacheHash));
}

// ---------------------------------------------------------------------------
//  getMarkedPosition — 0-based queue position if this cache hash's fic is in
//  Marked for Later, else -1. ViewEntry carries only a cache hash (kept
//  deliberately minimal, see Ao3ViewEntry.h), not the fic's file path, so this
//  resolves each of the (at most 10) marked entries' own paths to the same
//  hash-derived cache-path domain getBookStatus() above uses, rather than
//  needing the raw path here.
// ---------------------------------------------------------------------------

int Ao3LibraryActivity::getMarkedPosition(uint64_t cacheHash) {
  const std::string targetCachePath = "/.crosspoint/epub_" + std::to_string(cacheHash);
  const auto& entries = AO3_MARKED_FOR_LATER_STORE.getEntries();
  for (size_t i = 0; i < entries.size(); i++) {
    if (Epub::cachePathForFilePath(entries[i].path, "/.crosspoint") == targetCachePath) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
//  loadPageCache — pulls full Ao3LibraryMetadata for the visible 3 entries
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loadPageCache(int page) {
  const int startIdx = page * 3;
  const int endIdx = std::min(startIdx + 3, static_cast<int>(viewEntries.size()));

  // Zero all slots first
  for (int i = 0; i < 3; i++) {
    new (&pageCache[i]) Ao3LibraryMetadata();
    pageCacheStatus[i] = BookStatus::START;
    pageCacheMarkedPosition[i] = -1;
    pageCacheInfoMissing[i] = false;
    pageCacheUnindexed[i] = false;
  }

  for (int i = startIdx; i < endIdx; i++) {
    const int slot = i - startIdx;
    std::string infoPath = "/.crosspoint/epub_" + std::to_string(viewEntries[i].cacheHash) + "/ao3_library_info";
    HalFile f;
    bool infoLoaded = false;
    if (Storage.openFileForRead("AO3L", infoPath, f)) {
      infoLoaded = f.read((uint8_t*)&pageCache[slot], sizeof(Ao3LibraryMetadata)) == sizeof(Ao3LibraryMetadata);
      f.close();
    }
    infoLoaded = infoLoaded && pageCache[slot].isValid();
    if (!infoLoaded) new (&pageCache[slot]) Ao3LibraryMetadata();  // drop a short or stale read
    pageCacheInfoMissing[slot] = !infoLoaded;
    if (const UnindexedMarked* u = findUnindexedMarked(viewEntries[i].cacheHash)) {
      // The fic's own scraped info wins if it exists; otherwise show what the marked-for-later store knows.
      pageCacheUnindexed[slot] = !infoLoaded;
      strncpy(pageCache[slot].filepath, u->path.c_str(), sizeof(pageCache[slot].filepath) - 1);
      if (!infoLoaded) {
        strncpy(pageCache[slot].title, u->title.c_str(), sizeof(pageCache[slot].title) - 1);
        strncpy(pageCache[slot].author, u->author.c_str(), sizeof(pageCache[slot].author) - 1);
      }
    }
    pageCacheStatus[slot] = getBookStatus(viewEntries[i].cacheHash);
    pageCacheMarkedPosition[slot] = getMarkedPosition(viewEntries[i].cacheHash);
  }

  cachedPage = page;

  // Pre-compute wrapped summary lines — zero CPU work in render()
  const int textWidth = renderer.getScreenWidth() - 40;
  for (int i = 0; i < 3; i++) {
    wrappedSummary[i].clear();
    if (pageCache[i].summary[0] != 0) {
      for (int j = 0; pageCache[i].summary[j] != '\0'; j++) {
        if (pageCache[i].summary[j] == '\n' || pageCache[i].summary[j] == '\r') {
          pageCache[i].summary[j] = ' ';
        }
      }
      wrappedSummary[i] = renderer.wrappedText(SMALL_FONT_ID, pageCache[i].summary, textWidth, 3);
    }
  }
}

// ---------------------------------------------------------------------------
//  loop
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loop() {
  // Still loading — loadViewEntries() will flip indexState on first call
  if (indexState == IndexState::UNKNOWN) {
    // Fics sent through AO3 Receive: index them, and offer to replace any copy already here.
    if (!receivedReviewLaunched_ && Ao3ReceiveUtils::hasPending()) {
      receivedReviewLaunched_ = true;
      startActivityForResult(std::make_unique<Ao3ReceivedReviewActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    if (autoIndexOnOpen_ && !autoIndexLaunched_ && !ao3Folder.empty()) {
      bool full = false;
      {
        HalFile f;
        if (Storage.openFileForRead("AO3L", "/.crosspoint/ao3_library_index.bin", f)) {
          char magic[4];
          uint8_t version;
          uint16_t recordCount;
          // Reject the pre-fnvHash64 index format (version < 3): its records are a
          // different size, so reading them here would misalign (matches the same
          // guard in Ao3IndexActivity::buildIndexedHashes).
          if (f.read(magic, 4) == 4 && f.read(&version, 1) == 1 && f.read((uint8_t*)&recordCount, 2) == 2 &&
              memcmp(magic, "AO3X", 4) == 0 && version == 3 && recordCount <= MAX_INDEX_RECORDS) {
            f.seek(12);  // skip rest of header
            uint16_t liveCount = 0;
            CompactIndexRecord rec;
            for (uint16_t i = 0; i < recordCount; i++) {
              if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
              if (!(rec.flags & 0x01)) liveCount++;
            }
            full = (liveCount >= maxLibraryBooks());
          }
          f.close();
        }
      }
      if (pendingTransferScan && !full) {
        pendingTransferScan = false;  // Reset the flag so it won't scan again until a new book is added
        autoIndexLaunched_ = true;
        {
          // viewEntries is read by render()/renderLibrary() under its own
          // lock -- clear()/shrink_to_fit() from loop() need the same lock.
          RenderLock lock(*this);
          viewEntries.clear();
          viewEntries.shrink_to_fit();
        }
        auto handler = [this](const ActivityResult&) {
          indexState = IndexState::UNKNOWN;
          rebuildViewEntries();
          requestUpdate();
        };

        // Pass autoFinishIfEmpty = true, headless = true
        startActivityForResult(
            std::make_unique<Ao3IndexActivity>(renderer, mappedInput, Ao3IndexMode::DIRECTORY, "", true, true),
            handler);
        return;
      }
    }
    loadViewEntries();
    requestUpdate();
    return;
  }

  // A touch on a registered row sets this per-frame flag, which the mode's
  // existing Confirm handler honors. This avoids injectRelease(), which the main
  // loop only clears on the button-shortcut path (not the normal activity
  // dispatch), so an injected release would latch and fire Confirm every frame.
  bool confirmViaTap = false;

  // --- STATE: LIBRARY ---
  if (screenState == ScreenState::LIBRARY) {
    // Touch (X4 Pro): tappable back/filter/manage, tap-to-open, swipe paging.
    // Every touch query is a constexpr no-op on button-only builds
    // (CROSSINK_APP_CAP_TOUCH=0), so default/sticky stay button-only.
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      mappedInput.suppressCurrentTouchContact();
      if (skipNextBackRelease) {
        skipNextBackRelease = false;
      } else {
        activityManager.popActivity();
      }
      return;
    }
    // Tap a fic row (registered in renderLibrary) → open it directly. Consume the
    // touch contact so the tap can't re-fire or leak into the reader.
    {
      int tappedItem = -1;
      if (mappedInput.wasItemTapped(tappedItem) && tappedItem >= 0 &&
          tappedItem < static_cast<int>(viewEntries.size())) {
        mappedInput.suppressCurrentTouchContact();
        selectorIndex = static_cast<size_t>(tappedItem);
        const int selPage = tappedItem / 3;
        if (selPage != cachedPage) {
          // loadPageCache() reassigns pageCache/wrappedSummary, which render()
          // reads under its own RenderLock -- must not mutate them unlocked
          // from loop().
          RenderLock lock(*this);
          loadPageCache(selPage);
        }
        const std::string epubPath(pageCache[tappedItem % 3].filepath);
        if (!epubPath.empty()) {
          APP_STATE.ao3LibraryReturnIndex = tappedItem;
          activityManager.goToReader(epubPath);
        }
        return;
      }
    }
    {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int screenW = renderer.getScreenWidth();
      const int hintsY = renderer.getScreenHeight() - metrics.buttonHintsHeight;
      int tapX = 0;
      int tapY = 0;
      if (mappedInput.wasScreenTapped(tapX, tapY)) {
        // Header filter triangle (top-right) → open the filter panel.
        if (tapY < 48 && tapX >= screenW - 52) {
          mappedInput.suppressCurrentTouchContact();
          screenState = ScreenState::FILTER_PANEL;
          pendingState = activeState;
          overlayRowIndex = 0;
          requestUpdate(true);
          return;
        }
        // Bottom-right triangle in the hints bar → open the manage panel.
        if (tapY >= hintsY && tapX >= screenW - 52) {
          mappedInput.suppressCurrentTouchContact();
          screenState = ScreenState::MANAGE_PANEL;
          managePanelRowIndex = 0;
          requestUpdate(true);
          return;
        }
      }
    }
    if (!viewEntries.empty()) {
      const int total = static_cast<int>(viewEntries.size());
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        selectorIndex = swipe == MappedInputManager::SwipeDir::Up
                            ? ButtonNavigator::nextPageIndex(selectorIndex, total, 3)
                            : ButtonNavigator::previousPageIndex(selectorIndex, total, 3);
        requestUpdate();
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      screenState = ScreenState::MANAGE_PANEL;
      managePanelRowIndex = 0;
      requestUpdate(true);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      screenState = ScreenState::FILTER_PANEL;
      pendingState = activeState;
      overlayRowIndex = 0;
      requestUpdate(true);
      return;
    }
    const int total = static_cast<int>(viewEntries.size());
    if (total > 0) {
      // Tap: step one entry using Right and Left buttons only
      buttonNavigator.onPress({MappedInputManager::Button::Right}, [this, total] {
        selectorIndex = (selectorIndex + 1) % total;
        requestUpdate();
      });
      buttonNavigator.onPress({MappedInputManager::Button::Left}, [this, total] {
        selectorIndex = (selectorIndex + total - 1) % total;
        requestUpdate();
      });
      // Hold: jump a full page using Right and Left buttons only
      buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this, total] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, total, 3);
        requestUpdate();
      });
      buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this, total] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, total, 3);
        requestUpdate();
      });
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (skipNextBackRelease) {
        skipNextBackRelease = false;
      } else {
        activityManager.popActivity();
      }
      return;
    }

    if (!viewEntries.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Ensure the page cache is fresh for the current selector position
      const int selPage = static_cast<int>(selectorIndex) / 3;
      if (selPage != cachedPage) {
        // See the tap handler above: loadPageCache() must run under a
        // RenderLock when called from loop(), not just from render().
        RenderLock lock(*this);
        loadPageCache(selPage);
      }

      const int slot = static_cast<int>(selectorIndex) % 3;
      // Full epub path and title live in the ao3_library_info sidecar (page cache)
      const std::string epubPath(pageCache[slot].filepath);
      const std::string epubTitle(pageCache[slot].title[0] ? pageCache[slot].title : viewEntries[selectorIndex].title);
      const uint64_t hash = viewEntries[selectorIndex].cacheHash;

      if (mappedInput.getHeldTime() >= 1000 && !epubPath.empty()) {
        // Long-press → BookAction menu
        auto handler = [this, epubPath, hash](const ActivityResult& res) {
          if (const auto* actionRes = std::get_if<BookActionResult>(&res.data)) {
            if (actionRes->modified) {
              if (actionRes->deleted || actionRes->archived) {
                if (actionRes->deleted) {
                  // Tombstone in the index, remove file + cache from disk
                  Ao3Librarian::tombstoneRecord(epubPath);
                  if (Storage.remove(epubPath.c_str())) {
                    Epub(epubPath, "/.crosspoint").clearCache();
                  }
                }
                // Archiving already tombstoned the record and moved the
                // file/cache itself (Ao3ArchiveUtils::archiveFic) -- either
                // way the fic no longer belongs at this index, so just drop
                // it from the in-RAM view rather than leave a stale row
                // showing a bogus reset status. viewEntries is read by
                // render()/renderLibrary() under its own lock, so erasing
                // from it here (loop task) needs the same lock.
                RenderLock lock(*this);
                auto it = std::find_if(viewEntries.begin(), viewEntries.end(),
                                       [hash](const ViewEntry& v) { return v.cacheHash == hash; });
                if (it != viewEntries.end()) viewEntries.erase(it);

                // Clamp selectorIndex so we don't go out of bounds on next render
                if (!viewEntries.empty()) {
                  if (selectorIndex >= viewEntries.size()) {
                    selectorIndex = viewEntries.size() - 1;
                  }
                } else {
                  selectorIndex = 0;
                }
                cachedPage = -1;  // invalidate so next render reloads page cache
              } else if (actionRes->indexingCompleted || actionRes->restored ||
                         (actionRes->markedForLaterChanged && activeState.view == LibraryView::MARKED_FOR_LATER)) {
                // Restore re-scrapes a fresh live index record -- this fic's
                // cache hash/index slot may not match what's cached here
                // (it may not even have been a normal live entry a moment
                // ago), so a full rebuild is the safe choice rather than an
                // in-place patch. Marking/unmarking inside the Marked for Later
                // view changes which fics belong in the list at all.
                rebuildViewEntries();
              } else {
                // Status and/or Marked-for-Later change — update in-place
                // without a full reload. pageCacheStatus/pageCacheMarkedPosition
                // are read by render() under its own RenderLock, so mutating
                // them here (on the loop task) needs the same lock.
                if (static_cast<int>(selectorIndex) / 3 == cachedPage) {
                  RenderLock lock(*this);
                  pageCacheStatus[selectorIndex % 3] = actionRes->newStatus;
                  if (actionRes->markedForLaterChanged) {
                    // AO3_MARKED_FOR_LATER_STORE is a FIFO -- a mark/unmark
                    // shifts every other marked fic's queue position, so
                    // refresh every cached slot's badge, not just the
                    // acted-upon one, or a sibling row on this same page
                    // shows a stale position digit until the page reloads.
                    const int startIdx = cachedPage * 3;
                    const int endIdx = std::min(startIdx + 3, static_cast<int>(viewEntries.size()));
                    for (int i = startIdx; i < endIdx; i++) {
                      pageCacheMarkedPosition[i - startIdx] = getMarkedPosition(viewEntries[i].cacheHash);
                    }
                  } else {
                    pageCacheMarkedPosition[selectorIndex % 3] = getMarkedPosition(hash);
                  }
                }
              }
              requestUpdate(true);
            }
          }
        };
        startActivityForResult(std::make_unique<BookActionActivity>(renderer, mappedInput, epubPath, epubTitle),
                               handler);
      } else if (!epubPath.empty()) {
        APP_STATE.ao3LibraryReturnIndex = static_cast<int>(selectorIndex);
        activityManager.goToReader(epubPath);
      }
      return;
    }
  }

  // --- STATE: FILTER_PANEL ---
  else if (screenState == ScreenState::FILTER_PANEL) {
    // Touch (X4 Pro): tap a registered row / Confirm button; tap outside to close.
    {
      int tappedItem = -1;
      if (mappedInput.wasItemTapped(tappedItem) && tappedItem >= 0 && tappedItem < OVERLAY_ROW_COUNT) {
        mappedInput.suppressCurrentTouchContact();
        overlayRowIndex = tappedItem;  // rows 0..4, Confirm = 5
        confirmViaTap = true;
        // fall through to the Confirm handler below.
      } else {
        int tapX = 0;
        int tapY = 0;
        if (mappedInput.wasScreenTapped(tapX, tapY)) {
          if (tapY < OVERLAY_START_Y || tapY >= OVERLAY_START_Y + OVERLAY_HEIGHT) {
            mappedInput.suppressCurrentTouchContact();
            screenState = ScreenState::LIBRARY;  // tap outside the overlay closes it
            requestUpdate(true);
            return;
          }
        }
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      screenState = ScreenState::LIBRARY;
      requestUpdate(true);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (mappedInput.getHeldTime() >= 500) {
        // Skip to confirm
        overlayRowIndex = OVERLAY_ROW_CONFIRM;
      } else {
        // Move Next Row (wrapping around all rows, Confirm last)
        overlayRowIndex = (overlayRowIndex + 1) % OVERLAY_ROW_COUNT;
      }
      requestUpdate(true);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (mappedInput.getHeldTime() >= 500) {
        overlayRowIndex = OVERLAY_ROW_CONFIRM;
      } else {
        // Move Prev Row (wrapping around backward: 0 becomes Confirm)
        overlayRowIndex = (overlayRowIndex + OVERLAY_ROW_COUNT - 1) % OVERLAY_ROW_COUNT;
      }
      requestUpdate(true);
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || confirmViaTap) {
      if (overlayRowIndex == 0) {
        // Rating row: cycle Any -> G -> T -> M -> E -> Not Rated -> Any
        switch (pendingState.rating) {
          case 0:
            pendingState.rating = 'G';
            break;
          case 'G':
            pendingState.rating = 'T';
            break;
          case 'T':
            pendingState.rating = 'M';
            break;
          case 'M':
            pendingState.rating = 'E';
            break;
          case 'E':
            pendingState.rating = '-';
            break;
          default:
            pendingState.rating = 0;
            break;
        }
      } else if (overlayRowIndex == 1) {
        // Completion row (Automatic mode): cycle Any -> Complete -> Incomplete -> Any
        switch (pendingState.completion) {
          case -1:
            pendingState.completion = 1;
            break;
          case 1:
            pendingState.completion = 0;
            break;
          default:
            pendingState.completion = -1;
            break;
        }
      } else if (overlayRowIndex == 2) {
        // Sort by cycle: Title -> Author -> Word Count -> Date Added -> Series -> Title
        switch (pendingState.sortMode) {
          case SortMode::ALPHABETIC:
            pendingState.sortMode = SortMode::AUTHOR;
            break;
          case SortMode::AUTHOR:
            pendingState.sortMode = SortMode::WORD_COUNT;
            break;
          case SortMode::WORD_COUNT:
            pendingState.sortMode = SortMode::DATE_ADDED;
            break;
          case SortMode::DATE_ADDED:
            pendingState.sortMode = SortMode::SERIES;
            break;
          case SortMode::SERIES:
            pendingState.sortMode = SortMode::ALPHABETIC;
            break;
        }
      } else if (overlayRowIndex == 3) {
        // Order cycle
        pendingState.ascending = !pendingState.ascending;
      } else if (overlayRowIndex == OVERLAY_ROW_SHOW) {
        // Show cycle: All -> Marked for Later -> New Chapters -> WIPs -> All
        pendingState.view = pendingState.view == LibraryView::WIPS
                                ? LibraryView::ALL
                                : static_cast<LibraryView>(static_cast<uint8_t>(pendingState.view) + 1);
      } else if (overlayRowIndex == OVERLAY_ROW_CONFIRM) {
        // Confirm Button pressed
        SortFilterState previous = activeState;
        activeState = pendingState;
        screenState = ScreenState::LIBRARY;
        saveSortFilterState();
        applyStateChange(previous, activeState);
      }
      requestUpdate(true);
      return;
    }
  }

  // --- STATE: MANAGE_PANEL ---
  else if (screenState == ScreenState::MANAGE_PANEL) {
    // Touch (X4 Pro): tap a registered row to activate it; tap above the panel to close.
    {
      int tappedItem = -1;
      if (mappedInput.wasItemTapped(tappedItem) && (tappedItem == 0 || tappedItem == 1)) {
        mappedInput.suppressCurrentTouchContact();
        managePanelRowIndex = tappedItem;
        confirmViaTap = true;
        // fall through to the Confirm handler below.
      } else {
        const auto& metrics = UITheme::getInstance().getMetrics();
        const int panelH = 216;
        const int panelY = renderer.getScreenHeight() - panelH - metrics.buttonHintsHeight;
        int tapX = 0;
        int tapY = 0;
        if (mappedInput.wasScreenTapped(tapX, tapY) && tapY < panelY) {
          mappedInput.suppressCurrentTouchContact();
          screenState = ScreenState::LIBRARY;  // tap above the panel closes it
          requestUpdate(true);
          return;
        }
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      screenState = ScreenState::LIBRARY;
      requestUpdate(true);
      return;
    }

    // Up/Down/Left/Right all cycle between the 2 entries
    auto cycleNext = [this] {
      managePanelRowIndex = (managePanelRowIndex + 1) % 2;
      requestUpdate(true);
    };
    auto cyclePrev = [this] {
      managePanelRowIndex = (managePanelRowIndex + 1) % 2;
      requestUpdate(true);
    };

    if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      cycleNext();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      cyclePrev();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || confirmViaTap) {
      if (managePanelRowIndex == 0) {
        // "Index New Books" — check if ao3 folder is configured first
        const char* settingsPath = "/.crosspoint/ao3_settings.json";
        bool hasFolder = false;
        if (Storage.exists(settingsPath)) {
          String json = Storage.readFile(settingsPath);
          if (!json.isEmpty()) {
            JsonDocument doc;
            if (!deserializeJson(doc, json)) {
              std::string folder = doc["ao3Folder"] | "";
              hasFolder = !folder.empty();
            }
          }
        }
        if (!hasFolder) {
          // Redirect to settings
          screenState = ScreenState::MANAGE_PANEL;  // close panel first
          auto handler = [this](const ActivityResult&) {
            loadSettings();
            rebuildViewEntries();
            requestUpdate(true);
          };
          startActivityForResult(std::make_unique<Ao3LibrarySettingsActivity>(renderer, mappedInput), handler);
        } else {
          // Unload ViewEntry vector to free RAM, then launch Ao3IndexActivity
          viewEntries.clear();
          viewEntries.shrink_to_fit();
          indexState = IndexState::UNKNOWN;  // mark for rebuild on return
          screenState = ScreenState::LIBRARY;
          auto handler = [this](const ActivityResult&) {
            rebuildViewEntries();
            requestUpdate(true);
          };
          startActivityForResult(std::make_unique<Ao3IndexActivity>(renderer, mappedInput, Ao3IndexMode::DIRECTORY),
                                 handler);
        }
      } else {
        // "AO3 Library Settings"
        screenState = ScreenState::LIBRARY;
        auto handler = [this](const ActivityResult&) {
          loadSettings();
          rebuildViewEntries();
          requestUpdate(true);
        };
        startActivityForResult(std::make_unique<Ao3LibrarySettingsActivity>(renderer, mappedInput), handler);
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
//  render
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::render(RenderLock&& lock) {
  // MANAGE_PANEL is a full-screen composited: library background + bottom slide-up panel
  renderLibrary(lock);

  if (screenState == ScreenState::FILTER_PANEL) {
    renderFilterOverlay();
  } else if (screenState == ScreenState::MANAGE_PANEL) {
    renderManagePanel();
  }

  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
//  renderLibrary — renders standard book list (or empty messages)
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderLibrary(RenderLock& lock) {
  renderer.clearScreen();

  // Header title: the active view's name, else "AO3 Library"
  char headerTitle[32] = "AO3 Library";
  if (activeState.view != LibraryView::ALL) strcpy(headerTitle, viewLabel(activeState.view));
  renderer.drawText(UI_12_FONT_ID, 15, 12, headerTitle, true, EpdFontFamily::BOLD);
  renderer.drawLine(0, 48, renderer.getScreenWidth(), 48);

  // States handling
  if (indexState == IndexState::UNKNOWN) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, "Loading...");
    return;
  }
  // Triangle indicator in header — pointing down when filter closed, up when open, also up for MANAGE_PANEL
  {
    const int tx = renderer.getScreenWidth() - 26;
    const int ty = 26;
    if (screenState == ScreenState::FILTER_PANEL) {
      // triangle with black border pointing up (filter is open)
      const int xPts[] = {tx, tx + 12, tx + 6};
      const int yPts[] = {ty + 5, ty + 5, ty - 5};
      renderer.fillPolygon(xPts, yPts, 3, Black);
    } else {
      // Solid black triangle pointing down (filter accessible via Down button)
      const int xPts[] = {tx, tx + 12, tx + 6};
      const int yPts[] = {ty - 5, ty - 5, ty + 5};
      renderer.fillPolygon(xPts, yPts, 3, Black);
    }
  }

  if (indexState == IndexState::MISSING || (indexState == IndexState::OK && viewEntries.empty())) {
    if (indexState == IndexState::OK && activeState.view != LibraryView::ALL) {
      const char* none = activeState.view == LibraryView::MARKED_FOR_LATER ? tr(STR_NO_MARKED_FOR_LATER)
                         : activeState.view == LibraryView::NEW_CHAPTERS   ? tr(STR_NO_NEW_CHAPTERS)
                                                                            : tr(STR_NO_WIPS);
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, none);
      renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12,
                                "Open the filter and set Show to All.");
    } else if (activeState.rating != 0 || activeState.completion != -1) {
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, "No matches for current filter.");
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, "No AO3 books indexed yet.");
      renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12,
                                "Press SIDE UP to index new books.");
    }
  } else if (indexState == IndexState::CORRUPT) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 - 12, "Library index missing or corrupt.");
    renderer.drawCenteredText(SMALL_FONT_ID, renderer.getScreenHeight() / 2 + 12, "Open a book to recreate it.");
  } else {
    const int startIdx = (selectorIndex / 3) * 3;
    const int endIdx = std::min(startIdx + 3, static_cast<int>(viewEntries.size()));

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int topPad = 18;

    const int contentEnd = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    const int entrySlot = (contentEnd - (42 + topPad)) / 3;
    const int contentStart = 48 + topPad;

    const int currentPage = static_cast<int>(selectorIndex) / 3;
    if (currentPage != cachedPage) {
      loadPageCache(currentPage);
    }

    int y = contentStart;
    for (int i = startIdx; i < endIdx; i++) {
      const bool selected = (i == static_cast<int>(selectorIndex)) && (screenState == ScreenState::LIBRARY);
      renderEntry(lock, y, viewEntries[i], i - startIdx, selected);
      // Register the row as a tappable item (X4 Pro); no-op on button-only builds.
      // Only when LIBRARY is the active screen — when a panel overlays the list,
      // the library is a non-interactive background and must not register taps.
      if (screenState == ScreenState::LIBRARY) {
        TouchRegistry::getInstance().add(Rect{0, y, renderer.getScreenWidth(), entrySlot}, i, TouchRegistry::Item);
      }
      y += entrySlot;
      if (i < endIdx - 1) {
        renderer.drawLine(15, y - topPad, renderer.getScreenWidth() - 15, y - topPad);
      }
    }

    // Page counter
    if (static_cast<int>(viewEntries.size()) > 3) {
      char pageBuf[32];
      sprintf(pageBuf, "%d / %d", (startIdx / 3) + 1, (static_cast<int>(viewEntries.size()) + 2) / 3);
      const int counterX = renderer.getScreenWidth() - 36 - renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
      renderer.drawText(SMALL_FONT_ID, counterX, 15, pageBuf);
    }
  }

  // Draw Button hints

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Triangle indicator: Show 'Up' triangle when Manage Panel is closed
  if (screenState != ScreenState::MANAGE_PANEL) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int tx = managePanelTriangleX(renderer.getScreenWidth());
    const int hintsY = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    const int ty = hintsY + metrics.buttonHintsHeight / 2;

    // ▲ pointing up
    const int xPts[] = {tx + 6, tx, tx + 12};
    const int yPts[] = {ty - 5, ty + 5, ty + 5};
    renderer.fillPolygon(xPts, yPts, 3, Black);
  }
}

// ---------------------------------------------------------------------------
//  renderManagePanel — "Manage AO3 Library" slide-up panel from bottom
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderManagePanel() {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Panel dimensions: 216px tall, anchored to bottom above the hints bar
  const int panelH = 216;
  const int panelY = screenHeight - panelH - metrics.buttonHintsHeight;
  const int margin = 20;
  const int rowHeight = 52;
  const int rowGap = 8;

  // White filled background
  renderer.fillRoundedRect(0, panelY, screenWidth, panelH + metrics.buttonHintsHeight, 0, White);
  // Top 6px shadow line
  for (int i = 0; i < 6; i++) {
    renderer.drawLine(0, panelY + i, screenWidth, panelY + i);
  }

  // Panel header
  renderer.drawText(UI_12_FONT_ID, margin + 12, panelY + 22, "Manage AO3 Library", true, EpdFontFamily::BOLD);

  // Row definitions
  const char* rowLabels[2] = {"Index New Books", "Settings"};
  const char* rowDescs[2] = {"Scan AO3 folder & find unindexed epubs", "Set AO3 folder & Filter mode"};

  const int firstRowY = panelY + 62;
  for (int i = 0; i < 2; i++) {
    const int rowY = firstRowY + i * (rowHeight + rowGap);
    const bool sel = (managePanelRowIndex == i);

    if (sel) {
      renderer.fillRoundedRect(margin, rowY, screenWidth - 2 * margin, rowHeight, 6, true, true, true, true, LightGray);
    }

    renderer.drawText(UI_10_FONT_ID, margin + 12, rowY + 4, rowLabels[i], true);
    renderer.drawText(SMALL_FONT_ID, margin + 12, rowY + 26, rowDescs[i]);

    // Register the row as a tappable item (X4 Pro); no-op on button-only builds.
    TouchRegistry::getInstance().add(Rect{margin, rowY, screenWidth - 2 * margin, rowHeight}, i, TouchRegistry::Item);
  }

  // Button hints override for the panel
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Select", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Manage panel triangle indicator — in button hints bar, nudged in from the header triangle's X on
  // X4 Pro (see managePanelTriangleX()) to clear that board's recessed bezel.
  {
    const int tx = managePanelTriangleX(renderer.getScreenWidth());
    const int hintsY = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    const int ty = hintsY + metrics.buttonHintsHeight / 2;
    if (screenState == ScreenState::MANAGE_PANEL) {
      // ▼ pointing down when panel is open
      const int xPts[] = {tx, tx + 12, tx + 6};
      const int yPts[] = {ty - 5, ty - 5, ty + 5};
      renderer.fillPolygon(xPts, yPts, 3, Black);
    }
  }
}

void Ao3LibraryActivity::renderFilterOverlay() {
  const int screenWidth = renderer.getScreenWidth();
  const int overlayH = OVERLAY_HEIGHT;
  const int startY = OVERLAY_START_Y;

  // Background white filled rectangle
  renderer.fillRoundedRect(0, startY, screenWidth, overlayH, 0, White);
  // Top black edge
  renderer.fillRect(0, startY, screenWidth, 1, Black);
  // Bottom edge has a 6-pixel-thick black line
  for (int i = 0; i < 6; i++) {
    renderer.drawLine(0, startY + overlayH - 6 + i, screenWidth, startY + overlayH - 6 + i);
  }

  // Margins
  const int margin = 20;

  // Ubuntu 12pt Bold Header "Sort & Filter"
  renderer.drawText(UI_12_FONT_ID, margin + 12, startY + 20, "Sort & Filter", true, EpdFontFamily::BOLD);

  // helper to draw pill or label
  auto drawOverlayRow = [&](int rowIndex, const char* label, const char* val, bool disabled = false) {
    // Rows 0-1 are the filters, rows 2-3 the sort group (extra gap above it), row 4 the view selector.
    static constexpr int ROW_OFFSETS[] = {61, 101, 161, 201, 241};
    const int rowY = startY + ROW_OFFSETS[rowIndex];
    const bool isSelected = (overlayRowIndex == rowIndex);

    if (isSelected && !disabled) {
      renderer.fillRoundedRect(margin, rowY - 6, screenWidth - (2 * margin), 34, 6, true, true, true, true, LightGray);
    }

    // Label text
    renderer.drawText(UI_10_FONT_ID, margin + 12, rowY, label, !disabled);

    // Value text
    if (isSelected && !disabled) {
      // Draw black pill
      int valW = renderer.getTextWidth(UI_10_FONT_ID, val);
      int pillW = valW + 24;
      int pillX = screenWidth - margin - pillW;
      renderer.fillRoundedRect(pillX, rowY - 6, pillW, 34, 6, Black);
      renderer.drawText(UI_10_FONT_ID, pillX + 12, rowY - 1, val, false);
    } else {
      // Plain right-aligned value text
      int valW = renderer.getTextWidth(UI_10_FONT_ID, val);
      renderer.drawText(UI_10_FONT_ID, screenWidth - margin - valW - 12, rowY, val, !disabled);
    }

    // Register the row as a tappable item (X4 Pro); no-op on button-only builds.
    if (!disabled) {
      TouchRegistry::getInstance().add(Rect{margin, rowY - 6, screenWidth - 2 * margin, 34}, rowIndex,
                                       TouchRegistry::Item);
    }
  };

  // Row 0: Rating
  const char* ratingVal = "Any";
  switch (pendingState.rating) {
    case 'G':
      ratingVal = "General";
      break;
    case 'T':
      ratingVal = "Teen";
      break;
    case 'M':
      ratingVal = "Mature";
      break;
    case 'E':
      ratingVal = "Explicit";
      break;
    case '-':
      ratingVal = "Not Rated";
      break;
  }
  drawOverlayRow(0, "Rating", ratingVal);

  // Row 1: Completion
  const char* completionVal = pendingState.completion == 1   ? "Complete"
                              : pendingState.completion == 0 ? "Incomplete"
                                                             : "Any";
  drawOverlayRow(1, "Completion", completionVal);

  // Row 2: Sort By
  const char* sortLabel = "Title";
  switch (pendingState.sortMode) {
    case SortMode::ALPHABETIC:
      sortLabel = "Title";
      break;
    case SortMode::AUTHOR:
      sortLabel = "Author";
      break;
    case SortMode::WORD_COUNT:
      sortLabel = "Word Count";
      break;
    case SortMode::DATE_ADDED:
      sortLabel = "Date Added";
      break;
    case SortMode::SERIES:
      sortLabel = "Series";
      break;
  }
  drawOverlayRow(2, "Sort by", sortLabel);

  // Row 3: Order
  drawOverlayRow(3, "Order", pendingState.ascending ? "Ascending" : "Descending");

  // Row 4: Show (which slice of the library the list shows)
  drawOverlayRow(OVERLAY_ROW_SHOW, "Show", viewLabel(pendingState.view));

  // Row 5: Confirm Button
  const int btnY = startY + 290;
  const int btnW = screenWidth - margin * 2;
  const int btnH = 36;
  const bool confirmSelected = (overlayRowIndex == OVERLAY_ROW_CONFIRM);

  if (confirmSelected) {
    renderer.fillRoundedRect(margin, btnY, btnW, btnH, 8, Black);
    renderer.drawRoundedRect(margin, btnY, btnW, btnH, 1, 8, true);
    renderer.drawCenteredText(UI_12_FONT_ID, btnY + 2, "Confirm", false, EpdFontFamily::BOLD);
  } else {
    renderer.drawRoundedRect(margin, btnY, btnW, btnH, 1, 8, true);
    renderer.drawCenteredText(UI_12_FONT_ID, btnY + 2, "Confirm", true, EpdFontFamily::BOLD);
  }
  // Register the Confirm button as tappable item 5 (X4 Pro); no-op on button-only builds.
  TouchRegistry::getInstance().add(Rect{margin, btnY, btnW, btnH}, OVERLAY_ROW_CONFIRM, TouchRegistry::Item);
}

// ---------------------------------------------------------------------------
//  renderEntry
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderEntry(RenderLock& lock, int y, const ViewEntry& ve, int cacheSlot, bool selected) {
  const int margin = 20;
  const int selectionHeight = 56;
  const int squareSize = selectionHeight;
  const int textX = margin + squareSize + 15;

  if (selected) {
    renderer.fillRoundedRect(textX - 8, y - 3, renderer.getScreenWidth() - textX - 15, selectionHeight + 6, 8,
                             LightGray);
  }

  const Ao3LibraryMetadata& meta = pageCache[cacheSlot];
  const bool infoMissing = pageCacheInfoMissing[cacheSlot];
  const bool unindexed = pageCacheUnindexed[cacheSlot];

  // Without scraped info the index record still knows the rating and completion; a fic with no record at all
  // (unindexed) has neither, so its rating stays '-' and its completion is drawn as unknown.
  const char rating = infoMissing ? ve.rating : meta.rating;
  const bool completed = infoMissing ? static_cast<bool>(ve.isCompleted) : static_cast<bool>(meta.isCompleted);

  drawAo3Square(lock, margin, y, squareSize, rating, infoMissing, completed, pageCacheStatus[cacheSlot],
                pageCacheMarkedPosition[cacheSlot], unindexed);

  std::string title = meta.title[0] ? std::string(meta.title) : std::string(ve.title);
  std::string authorText = meta.author[0] ? std::string(meta.author) : std::string(ve.authorKey);

  if (meta.seriesName[0] != 0) {
    if (authorText.length() > 11) {
      authorText = authorText.substr(0, utf8SafeTruncateBuffer(authorText.c_str(), 11)) + ".";
    }
    char seriesBuf[256];
    if (meta.seriesPart > 0) {
      sprintf(seriesBuf, " \xE2\x80\xA2 %d of %s", meta.seriesPart, meta.seriesName);
    } else {
      sprintf(seriesBuf, " \xE2\x80\xA2 %s", meta.seriesName);
    }
    authorText += seriesBuf;
  }

  const int maxTextWidth = renderer.getScreenWidth() - textX - 25;

  auto truncateToFit = [&](std::string& text, int fontId, EpdFontFamily::Style style) {
    if (renderer.getTextWidth(fontId, text.c_str(), style) > maxTextWidth) {
      while (!text.empty() && renderer.getTextWidth(fontId, (text + "..").c_str(), style) > maxTextWidth) {
        while (!text.empty()) {
          const char c = text.back();
          text.pop_back();
          if ((c & 0xC0) != 0x80) break;
        }
      }
      text += "..";
    }
  };

  truncateToFit(title, UI_12_FONT_ID, EpdFontFamily::BOLD);
  truncateToFit(authorText, UI_10_FONT_ID, EpdFontFamily::REGULAR);

  renderer.drawText(UI_12_FONT_ID, textX, y + 6, title.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, textX, y + 32, authorText.c_str());

  if (infoMissing) {
    renderer.drawText(SMALL_FONT_ID, margin, y + selectionHeight + 12,
                      unindexed ? tr(STR_NOT_INDEXED) : tr(STR_DETAILS_UNAVAILABLE));
  } else {
    int blockY = y + selectionHeight + 12;

    // Tags
    int tagX = margin;
    for (int j = 0; j < 4; j++) {
      if (meta.tags[j][0] == 0) break;
      const int tagW = renderer.getTextWidth(SMALL_FONT_ID, meta.tags[j]) + 16;
      if (tagX + tagW > renderer.getScreenWidth() - margin) break;
      renderer.drawRoundedRect(tagX, blockY, tagW, 20, 1, 6, true);
      renderer.drawText(SMALL_FONT_ID, tagX + 8, blockY - 2, meta.tags[j]);
      tagX += tagW + 8;
    }
    blockY += 28;

    for (const auto& line : wrappedSummary[cacheSlot]) {
      renderer.drawText(SMALL_FONT_ID, margin, blockY, line.c_str());
      blockY += 20;
    }
    blockY += 10;

    char chaptersBuf[24];
    if (meta.totalChapters > 0) {
      snprintf(chaptersBuf, sizeof(chaptersBuf), "%d/%d", meta.chapterCount, meta.totalChapters);
    } else {
      snprintf(chaptersBuf, sizeof(chaptersBuf), "%d/?", meta.chapterCount);
    }

    char wordsBuf[17];
    if (meta.wordCount > 0) {
      snprintf(wordsBuf, sizeof(wordsBuf), "%s%lu", meta.wordCountIsEstimate ? "~" : "", (unsigned long)meta.wordCount);
    } else {
      snprintf(wordsBuf, sizeof(wordsBuf), "?");
    }

    char metaBuf[128];
    if (meta.updatedDate[0] != '\0') {
      sprintf(metaBuf, "Chapters: %s   Words: %s   Updated: %s", chaptersBuf, wordsBuf, meta.updatedDate);
    } else {
      sprintf(metaBuf, "Chapters: %s   Words: %s", chaptersBuf, wordsBuf);
    }
    renderer.drawText(SMALL_FONT_ID, margin, blockY, metaBuf);
  }
}

// ---------------------------------------------------------------------------
//  drawAo3Square
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::drawAo3Square(RenderLock& lock, int x, int y, int s, char rating, bool detailsMissing,
                                       bool completed, BookStatus status, int markedPosition, bool completionUnknown) {
  const int h = s / 2;

  renderSymbol(x + 1, y + 1, h - 1, rating, true, false, false, false, -1);
  renderStatusSymbol(x + h + 1, y + 1, h - 1, status, false, true, false, false, -1, markedPosition);
  renderDetailsSymbol(x + 1, y + h + 1, h - 1, detailsMissing, false, false, true, false, -2);
  if (completionUnknown) {
    renderSymbol(x + h + 1, y + h + 1, h - 1, '?', false, false, false, true, -2);  // not indexed: completion unknown
  } else {
    renderCompletionSymbol(x + h + 1, y + h + 1, h - 1, completed, false, false, false, true, -2);
  }
  renderer.drawRoundedRect(x, y, s, s, 1, 6, true);
  renderer.drawLine(x + 1, y + h, x + s - 1, y + h);
  renderer.drawLine(x + h, y + 1, x + h, y + s - 1);
}

// ---------------------------------------------------------------------------
//  Symbol Renderers
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::renderSymbol(int x, int y, int s, char c, bool tl, bool tr, bool bl, bool br, int yOffset) {
  Color bg = White;
  if (c == 'T') bg = LightGray;
  if (c == 'M') bg = DarkGray;
  if (c == 'E') bg = Black;
  if (bg != White) renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);
  char buf[2] = {c, 0};
  if (c == '-' || c == 0) buf[0] = '-';
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, buf);
  const int th = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + (s - tw) / 2, y + (s - th) / 2 + yOffset, buf,
                    (bg == DarkGray || bg == Black) ? false : true);
}

void Ao3LibraryActivity::renderStatusSymbol(int x, int y, int s, BookStatus status, bool tl, bool tr, bool bl, bool br,
                                            int yOffset, int markedPosition) {
  // Marked for Later takes priority over the regular R/F/chapter-status
  // symbol -- a queue position is more useful at a glance than a reading
  // status the reader hasn't acted on yet.
  if (markedPosition >= 0) {
    const std::string queueNum = std::to_string(markedPosition + 1);
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, queueNum.c_str());
    const int th = renderer.getTextHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, x + (s - tw) / 2, y + (s - th) / 2 + yOffset, queueNum.c_str(), true);
    return;
  }

  // Handle geometric custom renders for chapter status updates
  if (status == BookStatus::WAITING_FOR_CHAPTER || status == BookStatus::NEW_CHAPTER_AVAILABLE) {
    // 1. Calculate an upward-pointing triangle centered inside the quadrant
    int triW = s / 2;
    int triH = s / 2;
    int triX = x + (s - triW) / 2;
    int triY = y + (s - triH) / 2 + yOffset;

    int xPts[] = {triX + triW / 2, triX, triX + triW};
    int yPts[] = {triY, triY + triH, triY + triH};

    // Draw the black filled triangle (Both statuses get this on a white background)
    renderer.fillPolygon(xPts, yPts, 3, Black);

    // 2. If a new chapter is available, add the 9x9 circle notification badge
    if (status == BookStatus::NEW_CHAPTER_AVAILABLE) {
      int dotW = 11;
      int dotH = 10;
      // Positioned with a 1-pixel margin inside the top-right corner radius padding
      int dotX = x + s - 6;
      int dotY = y - 3;

      // A corner radius of 4 on a 9x9 square forces a perfect circle primitive
      renderer.fillRoundedRect(dotX, dotY, dotW, dotH, 4, true, true, true, true, Black);
    }
    return;  // Early exit so we don't fall back to font text rendering
  }

  // --- Fallback text rendering for standard statuses (READING, FINISHED, etc.) ---
  const char* txt = "-";
  Color bg = White;
  if (status == BookStatus::READING) {
    bg = LightGray;
    txt = "R";
  }
  if (status == BookStatus::FINISHED) {
    bg = Black;
    txt = "F";
  }

  if (bg != White) renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, txt);
  const int th = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + (s - tw) / 2, y + (s - th) / 2 + yOffset, txt, (bg == Black) ? false : true);
}

// Bottom-left square: '?' when the fic's details are missing (unindexed, or its scraped info could not be read),
// otherwise blank. (It used to show the AO3 archive warning.)
void Ao3LibraryActivity::renderDetailsSymbol(int x, int y, int s, bool missing, bool tl, bool tr, bool bl, bool br,
                                             int yOffset) {
  if (!missing) return;
  renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, DarkGray);
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, "?");
  const int th = renderer.getTextHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x + (s - tw) / 2, y + (s - th) / 2 + yOffset, "?", false);
}

void Ao3LibraryActivity::renderCompletionSymbol(int x, int y, int s, bool completed, bool tl, bool tr, bool bl, bool br,
                                                int yOffset) {
  const Color bg = completed ? LightGray : Black;
  renderer.fillRoundedRect(x, y, s, s, 6, tl, tr, bl, br, bg);

  if (completed) {
    // Draw a sharp 3px thick black checkmark using line primitives
    for (int dy = 0; dy < 4; dy++) {
      // Left short downward stroke
      renderer.drawLine(x + 8, y + 14 + yOffset + dy, x + 12, y + 18 + yOffset + dy);
      // Right long upward stroke
      renderer.drawLine(x + 12, y + 18 + yOffset + dy, x + 19, y + 11 + yOffset + dy);
    }
  } else {
    // Draw a 4px thick cross (X) using white line primitives
    // The 5th parameter (4) sets the thickness, and the 6th parameter (false) forces it to draw White

    // Line 1: Top-Left to Bottom-Right
    renderer.drawLine(x + 7, y + 8 + yOffset, x + 18, y + 18 + yOffset, 4, false);

    // Line 2: Bottom-Left to Top-Right
    renderer.drawLine(x + 7, y + 18 + yOffset, x + 18, y + 8 + yOffset, 4, false);
  }
}

// ---------------------------------------------------------------------------
//  Sort & Filter States Load/Save
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::loadSortFilterState() {
  activeState.rating = 0;
  activeState.completion = -1;
  activeState.sortMode = SortMode::ALPHABETIC;
  activeState.ascending = true;
  activeState.view = LibraryView::ALL;

  const char* path = "/.crosspoint/ao3SortFilterState.json";
  if (!Storage.exists(path)) return;

  String json = Storage.readFile(path);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  activeState.rating = static_cast<char>(doc["rating"] | 0);
  activeState.completion = static_cast<int8_t>(doc["completion"] | -1);
  activeState.sortMode = static_cast<SortMode>(doc["sortMode"] | 0);
  activeState.ascending = doc["ascending"] | true;

  if (activeState.sortMode > SortMode::AUTHOR) activeState.sortMode = SortMode::ALPHABETIC;

  const uint8_t persistedView = doc["view"] | 0;
  activeState.view = persistedView <= static_cast<uint8_t>(LibraryView::WIPS) ? static_cast<LibraryView>(persistedView)
                                                                              : LibraryView::ALL;
}

void Ao3LibraryActivity::saveSortFilterState() const {
  JsonDocument doc;
  doc["rating"] = static_cast<int>(activeState.rating);
  doc["completion"] = static_cast<int>(activeState.completion);
  doc["sortMode"] = static_cast<uint8_t>(activeState.sortMode);
  doc["ascending"] = activeState.ascending;
  doc["view"] = static_cast<uint8_t>(activeState.view);

  String json;
  serializeJson(doc, json);
  Storage.writeFile("/.crosspoint/ao3SortFilterState.json", json);
}

// ---------------------------------------------------------------------------
//  Sorting Logic
// ---------------------------------------------------------------------------

void Ao3LibraryActivity::resortViewEntries() {
  // viewEntries is read by render()/renderLibrary() under its own lock, and
  // this is called from several loop()-task sites (filter/sort confirm,
  // rebuildViewEntries() below) with no lock of their own -- an in-place
  // std::sort swapping ViewEntry structs (which contain strings) racing a
  // concurrent locked read is UB, not just a stale ordering. RenderLock is
  // recursive, so this is safe to call from a caller that already holds one.
  RenderLock lock(*this);
  if (isStoreView()) {
    // Queue / most-recently-updated order is the point of these views (and the queue-position
    // badge would read as scrambled if re-sorted), so Sort By / Order don't apply.
    std::sort(viewEntries.begin(), viewEntries.end(), [this](const ViewEntry& a, const ViewEntry& b) {
      return viewPosition(a.cacheHash) < viewPosition(b.cacheHash);
    });
    return;
  }
  switch (activeState.sortMode) {
    case SortMode::ALPHABETIC:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        int cmp = strncasecmp(a.title, b.title, 12);
        return activeState.ascending ? cmp < 0 : cmp > 0;
      });
      break;

    case SortMode::WORD_COUNT:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        if (a.wordCount != b.wordCount)
          return activeState.ascending ? a.wordCount < b.wordCount : a.wordCount > b.wordCount;
        return strncmp(a.title, b.title, 12) < 0;
      });
      break;

    case SortMode::DATE_ADDED:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        if (a.addedSequence != b.addedSequence)
          return activeState.ascending ? a.addedSequence < b.addedSequence : a.addedSequence > b.addedSequence;
        return strncmp(a.title, b.title, 12) < 0;
      });
      break;

    case SortMode::SERIES:
      // Alphabetical by series name (first 7 chars, like Author), a series' books kept together
      // by the full-name hash and ordered by part. Fics with no series always go last.
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        bool aHas = a.seriesHash != 0;
        bool bHas = b.seriesHash != 0;
        if (aHas != bHas) return aHas > bHas;  // no-series goes last

        if (aHas) {
          const int keyCmp = strncmp(a.seriesKey, b.seriesKey, 8);
          if (keyCmp != 0) return activeState.ascending ? keyCmp < 0 : keyCmp > 0;
          if (a.seriesHash != b.seriesHash) return a.seriesHash < b.seriesHash;
          if (a.seriesPart != b.seriesPart) return a.seriesPart < b.seriesPart;  // always ascending seriesPart
        }

        return strncmp(a.title, b.title, 12) < 0;
      });
      break;

    case SortMode::AUTHOR:
      std::sort(viewEntries.begin(), viewEntries.end(), [&](const ViewEntry& a, const ViewEntry& b) {
        int cmp = strncmp(a.authorKey, b.authorKey, 8);
        if (cmp != 0) return activeState.ascending ? cmp < 0 : cmp > 0;
        return strncmp(a.title, b.title, 12) < 0;
      });
      break;
  }
}

// ---------------------------------------------------------------------------
//  Filtering Logic
// ---------------------------------------------------------------------------

const Ao3LibraryActivity::UnindexedMarked* Ao3LibraryActivity::findUnindexedMarked(uint64_t cacheHash) const {
  for (const auto& u : unindexedMarked_) {
    if (u.cacheHash == cacheHash) return &u;
  }
  return nullptr;
}

int Ao3LibraryActivity::viewPosition(uint64_t cacheHash) const {
  for (size_t i = 0; i < viewOrder_.size(); i++) {
    if (viewOrder_[i] == cacheHash) return static_cast<int>(i);
  }
  return -1;
}

// Resolves the active store view's fics to the same cache hashes the index records carry
// (fnv of the file path, the same derivation ZipFile::fnvHash64 gives every cache dir).
void Ao3LibraryActivity::loadViewOrder() {
  viewOrder_.clear();
  auto add = [this](const std::string& path) {
    viewOrder_.push_back(ZipFile::fnvHash64(path.c_str(), path.size()));
  };
  if (activeState.view == LibraryView::MARKED_FOR_LATER) {
    for (const auto& e : AO3_MARKED_FOR_LATER_STORE.getEntries()) add(e.path);
  } else if (activeState.view == LibraryView::NEW_CHAPTERS) {
    for (const auto& e : AO3_NEW_CHAPTERS_STORE.getEntries()) add(e.path);
  }
}

bool Ao3LibraryActivity::passesFilter(const ViewEntry& v, const FilterHashes& h) const {
  if (activeState.view == LibraryView::WIPS && v.isCompleted) return false;
  if (isStoreView() && viewPosition(v.cacheHash) < 0) return false;

  if (h.ratingActive && v.rating != h.ratingValue) return false;
  if (h.completionActive && static_cast<bool>(v.isCompleted) != h.completionValue) return false;
  return true;
}

void Ao3LibraryActivity::rebuildViewEntries() {
  // viewEntries is read by render()/renderLibrary() under its own lock, and
  // this is called from several loop()-task sites (initial load, filter
  // confirm, post-index-return callbacks) with no lock of their own --
  // clear()+push_back() can reallocate the vector's backing store, which
  // races a concurrent locked read. RenderLock is recursive, so holding it
  // across this function's SD reads (matching loadPageCache()'s established
  // pattern elsewhere in this codebase) is safe even if a caller already
  // holds one.
  RenderLock lock(*this);
  viewEntries.clear();
  unindexedMarked_.clear();
  const char* indexPath = "/.crosspoint/ao3_library_index.bin";

  if (!Storage.exists(indexPath)) {
    indexState = IndexState::MISSING;
    return;
  }

  HalFile f;
  if (!Storage.openFileForRead("AO3L", indexPath, f)) {
    indexState = IndexState::CORRUPT;
    return;
  }

  char magic[4];
  uint8_t version;
  uint16_t recordCount;
  uint32_t nextSequence;
  uint8_t reserved;

  const bool readOk = f.read(magic, 4) == 4 && f.read(&version, 1) == 1 && f.read((uint8_t*)&recordCount, 2) == 2 &&
                      f.read((uint8_t*)&nextSequence, 4) == 4 && f.read(&reserved, 1) == 1;

  if (!readOk || memcmp(magic, "AO3X", 4) != 0 || version != 3) {
    f.close();
    indexState = IndexState::CORRUPT;
    return;
  }

  if (recordCount > MAX_INDEX_RECORDS) {
    f.close();
    indexState = IndexState::CORRUPT;
    return;
  }

  loadViewOrder();
  const FilterHashes filterHashes = computeFilterHashes(activeState);
  viewEntries.reserve(std::min<size_t>(recordCount, maxLibraryBooks()));

  // Shared with the regular Library's "Finished Books" toggle -- one flag, read directly rather
  // than through ao3_settings.json (which no longer carries its own copy of this setting).
  const bool hideFinished = SETTINGS.libraryHideFinishedBooks != 0;

  // Which of the Marked for Later fics have a live index record (whatever the filters then say about them).
  const bool markedView = activeState.view == LibraryView::MARKED_FOR_LATER;
  std::vector<uint8_t> markedInIndex(markedView ? viewOrder_.size() : 0, 0);

  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    if (rec.flags & 0x01) continue;  // Skip tombstone
    if (markedView) {
      const int pos = viewPosition(rec.cacheHash);
      if (pos >= 0) markedInIndex[pos] = 1;
    }
    if (hideFinished && (rec.flags & 0x02)) continue;  // Skip finished
    ViewEntry v = buildViewEntry(rec);
    if (passesFilter(v, filterHashes)) {
      viewEntries.push_back(v);
      // An index written by a device with more RAM: show what this one can hold.
      if (viewEntries.size() >= maxLibraryBooks()) break;
    }
    yield();
  }
  f.close();

  if (markedView) addUnindexedMarked(markedInIndex, filterHashes, hideFinished);

  resortViewEntries();
  indexState = IndexState::OK;
  cachedPage = -1;
  buttonsSetup = false;

  // The list may be shorter than when selectorIndex was set (returning to a New Chapters view
  // after opening the fic drops it from the list, a Marked for Later toggle, ...).
  if (selectorIndex >= viewEntries.size()) selectorIndex = viewEntries.empty() ? 0 : viewEntries.size() - 1;
}

// Adds the Marked for Later fics that have no live index record, so the view lists every marked fic. They carry no
// rating/completion, so an active rating or completion filter (which they cannot satisfy) hides them; the folder
// filter and Hide Finished apply as for indexed fics. Called with the render lock held (from rebuildViewEntries).
void Ao3LibraryActivity::addUnindexedMarked(const std::vector<uint8_t>& markedInIndex, const FilterHashes& filters,
                                            const bool hideFinished) {
  if (filters.ratingActive || filters.completionActive) return;
  const auto& entries = AO3_MARKED_FOR_LATER_STORE.getEntries();
  for (size_t i = 0; i < entries.size() && i < markedInIndex.size(); i++) {
    if (markedInIndex[i]) continue;
    const auto& e = entries[i];
    const uint64_t hash = viewOrder_[i];
    if (!Storage.exists(e.path.c_str())) continue;
    if (hideFinished &&
        Ao3Librarian::getBookStatus(Epub::cachePathForFilePath(e.path, "/.crosspoint")) == BookStatus::FINISHED) {
      continue;
    }
    ViewEntry v{};
    strncpy(v.title, e.title.c_str(), sizeof(v.title) - 1);
    strncpy(v.authorKey, e.author.c_str(), sizeof(v.authorKey) - 1);
    for (char* p = v.authorKey; *p; p++) {
      if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
    v.cacheHash = hash;
    v.rating = '-';
    viewEntries.push_back(v);
    unindexedMarked_.push_back({hash, e.path, e.title, e.author});
  }
}

void Ao3LibraryActivity::applyStateChange(const SortFilterState& prev, const SortFilterState& next) {
  bool filterChanged = prev.rating != next.rating || prev.completion != next.completion || prev.view != next.view;

  bool sortChanged = prev.sortMode != next.sortMode || prev.ascending != next.ascending;

  if (filterChanged) {
    rebuildViewEntries();
  } else if (sortChanged) {
    resortViewEntries();
  }

  selectorIndex = 0;
  cachedPage = -1;
  requestUpdate();
}
