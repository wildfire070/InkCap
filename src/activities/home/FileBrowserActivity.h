#pragma once

#include <FileIndex.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "BookStatus.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/SortPopup.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Picker modes return their selected path via ActivityResult.
  enum class Mode { Books, PickFirmware, PickDirectory };

  // Metadata-based sort fields for Mode::Books, offered via SortPopup. Only applies to
  // the fully-materialized `files` list (see usingIndex below) -- oversized folders keep
  // the existing fixed alphabetical order regardless of SETTINGS.fileBrowserSortField.
  enum class SortField : uint8_t { Title = 0, Author = 1, Status = 2, Rating = 3, Chapters = 4, DateUpdated = 5, LastOpened = 6 };
  static constexpr int SORT_FIELD_COUNT = 7;

 private:
  // FreeInkApp hosts the file list (themed rows, icons, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  // Deletion
  void promptDeleteFile(const std::string& fullPath, const std::string& entry);
  void promptDeleteDirectory(const std::string& fullPath, const std::string& entry,
                             bool ignoreInitialConfirmRelease = false);
  void showDirectoryActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease = false);
  void pinSleepFavorite(const std::string& fullPath);
  void unpinSleepFavorite();
  bool isPinnedSleepFavorite(const std::string& fullPath) const;
  void setPreferredSleepFolder(const std::string& fullPath);
  void clearPreferredSleepFolder();
  bool isPreferredSleepFolder(const std::string& fullPath) const;
  bool isSleepFavoriteFolder(const std::string& fullPath) const;
  void showFileActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease = false);
  // Nearest non-folder row to `fromRow` moving toward Left/Right's own direction
  // (forward=true is Right/next), skipping folders; SIZE_MAX if none -- used for
  // Book Info's Previous/Next Book, via entryNameAt()/entryCount() so it works
  // the same whether or not the folder is large enough to be in index mode.
  size_t findAdjacentBookRow(size_t fromRow, bool forward);
  // Opens Book Info for the file at `row`, wiring Left/Right = Previous/Next Book
  // via findAdjacentBookRow() above.
  void openBookDetails(size_t row);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool showFileSelection = true;

  bool lockLongPressBack = false;
  bool longPressBackHandled = false;
  bool longPressConfirmHandled = false;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Books;

  // Files state
  static constexpr size_t INDEX_ROW_CACHE_SIZE = 32;
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;
  std::unique_ptr<FileIndex> fileIndex;
  std::unique_ptr<FileIndex::Entry> indexEntry;
  std::array<std::string, INDEX_ROW_CACHE_SIZE> indexCachedNames;
  std::array<size_t, INDEX_ROW_CACHE_SIZE> indexCachedRows{};
  bool usingIndex = false;
  bool fileListMemoryLimited = false;
  bool fileListReadFailed = false;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  // The current FreeInkUI action payload is int16_t. For larger folders the
  // renderer supplies a local row number and this records its absolute base.
  size_t actionWindowFirst = 0;
  freeink::ui::ListNav listNav;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSettingsEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void activateSelected();
  void navigateBack();
  void openSettings();

  // Data loading
  void clearIndexNameCache();
  void loadFiles();
  // Caller holds RenderLock while replacing the list backing storage together
  // with its associated navigation state.
  void loadFilesLocked();
  bool loadFilesIntoVector(size_t cap, bool& overflow);
  size_t entryCount() const;
  const char* entryNameAt(size_t row);
  void toggleHiddenFiles();
  size_t findEntry(const std::string& name);

  // AO3 library: cheap glance status (Reading/Finished/Waiting for Chapter/...)
  // read from each book's own progress.bin, cached per visible row index.
  BookStatus getBookStatus(const std::string& path);
  std::map<size_t, BookStatus> visibleStatusCache;

  // Metadata sort (Mode::Books only). `files` already interleaves folders (natural-sorted,
  // pinned first) and non-folder entries; sortFiles() re-derives that ordering, replacing
  // FsHelpers::sortFileList's plain alphabetical pass with a metadata-keyed one when
  // SETTINGS.fileBrowserSortField is set. Keyed by entry name (not index) since indices
  // shift on every resort; cleared in loadFilesLocked() for a fresh folder.
  SortPopup sortPopup;
  Rect sortButtonRect{0, 0, 0, 0};
  // PageForward shares its physical button with the Power+Down screenshot
  // combo (see ButtonShortcutController::updatePowerDown); the two presses
  // rarely land in the same debounce window, so a screenshot attempt's Down
  // edge can register -- and open Sort -- a frame or two before Power does.
  // 0 = no pending Sort trigger; otherwise the millis() timestamp PageForward
  // was pressed, held for PAGE_FORWARD_SORT_GUARD_MS before actually opening
  // Sort, so a Power press arriving in that window can cancel it instead.
  unsigned long pendingSortFromPageForwardMs = 0;
  std::map<std::string, std::string> sortKeyCache[SORT_FIELD_COUNT];
  bool sortCacheReady[SORT_FIELD_COUNT] = {};
  void openSortPopup();
  void sortFiles();
  // Empty return means "no value for this field" -- always sorts to the end, regardless
  // of direction, matching how every other missing-metadata case in this app is handled.
  std::string computeSortKey(SortField field, const std::string& fullPath);
  void ensureSortCache(SortField field, const std::vector<std::string>& nonDirEntries);

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
