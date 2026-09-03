#include "FileBrowserActivity.h"

#include <Arduino.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "BookActions.h"
#include "BookDetailsActivity.h"
#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "activities/boot_sleep/SleepImageIndex.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long COMPLETED_FEEDBACK_MS = 1000;
constexpr int ROOT_HINT_GAP = 20;
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SETTINGS = 2;
constexpr size_t INDEX_THRESHOLD = 200;
constexpr size_t MAX_VIRTUAL_LIST_ENTRIES = static_cast<size_t>(std::numeric_limits<int16_t>::max());
constexpr uint32_t FILE_BROWSER_APPEND_MIN_FREE_AFTER_ALLOC = 48U * 1024U;
constexpr uint32_t FILE_BROWSER_APPEND_MIN_MAX_ALLOC_AFTER_ALLOC = 16U * 1024U;

// Persistent "Sort" edge tab (see the header/render-time comment in
// FileBrowserActivity::render() for the full rationale). Shared between
// render() (draws it) and buildListScreen() (reserves room for it in the list
// rect so a wide tab can't cover a row's own right edge / value column).
constexpr int kSortTabPadding = 6;
constexpr int kSortTabWidth = 30;
constexpr int kSortTabTopMargin = 4;
constexpr int kSortTabCornerRadius = 6;

bool usesTwoLineFileBrowserRows() {
  return SETTINGS.fileBrowserDisplay == CrossPointSettings::FILE_BROWSER_DISPLAY_2_LINES;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.length() != b.length()) return false;
  for (size_t i = 0; i < a.length(); ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool isDefaultSleepFolderPath(const std::string& path) {
  return equalsIgnoreCase(path, "/sleep") || equalsIgnoreCase(path, "/.sleep");
}

bool isSleepImageFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

bool isMacOSMetadataEntry(std::string_view filename) {
  return filename.rfind("._", 0) == 0 || filename == ".DS_Store" || filename == ".Spotlight-V100" ||
         filename == ".Trashes" || filename == ".fseventsd";
}

bool isWindowsMetadataEntry(std::string_view filename) {
  return equalsIgnoreCase(filename, "System Volume Information") || equalsIgnoreCase(filename, "$RECYCLE.BIN") ||
         equalsIgnoreCase(filename, "desktop.ini") || equalsIgnoreCase(filename, "Thumbs.db") ||
         equalsIgnoreCase(filename, "IndexerVolumeGuid") || equalsIgnoreCase(filename, "WPSettings.dat");
}

size_t estimateNextVectorCapacity(size_t size, size_t capacity) {
  if (size < capacity) {
    return capacity;
  }
  if (capacity == 0) {
    return 1;
  }
  return capacity * 2;
}

bool hasHeapForFileEntryAppend(const std::vector<std::string>& files, size_t entryLen) {
  const size_t nextCapacity = estimateNextVectorCapacity(files.size(), files.capacity());
  const uint32_t vectorGrowthBytes =
      (nextCapacity == files.capacity()) ? 0U : static_cast<uint32_t>(nextCapacity * sizeof(std::string));
  const uint32_t stringBytes = static_cast<uint32_t>(entryLen + 1);
  const uint32_t largestNeeded = std::max(vectorGrowthBytes, stringBytes);

  return ESP.getFreeHeap() >= vectorGrowthBytes + stringBytes + FILE_BROWSER_APPEND_MIN_FREE_AFTER_ALLOC &&
         ESP.getMaxAllocHeap() >= largestNeeded + FILE_BROWSER_APPEND_MIN_MAX_ALLOC_AFTER_ALLOC;
}

bool hasFileMetadata(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

bool isSupportedBrowserFile(std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
         FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename);
}

bool acceptCommon(const char* name, bool isDir) {
  if (isMacOSMetadataEntry(name) || isWindowsMetadataEntry(name) || (!SETTINGS.showHiddenFiles && name[0] == '.')) {
    return false;
  }
  return isDir || isSupportedBrowserFile(name);
}

bool acceptFirmware(const char* name, bool isDir) {
  if (isMacOSMetadataEntry(name) || isWindowsMetadataEntry(name) || (!SETTINGS.showHiddenFiles && name[0] == '.')) {
    return false;
  }
  return isDir || FsHelpers::checkFileExtension(std::string_view{name}, ".bin");
}

bool acceptDirectory(const char* name, const bool isDir) {
  return isDir && !isMacOSMetadataEntry(name) && !isWindowsMetadataEntry(name) &&
         (SETTINGS.showHiddenFiles || name[0] != '.');
}

std::string buildFullPath(std::string basepath, const std::string& entry) {
  if (basepath.back() != '/') basepath += "/";
  return basepath + entry;
}

std::string normalizeDirectoryPath(std::string path) {
  while (path.length() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

bool isSleepFolderPreferencePath(const std::string& path) { return !path.empty() && !isDefaultSleepFolderPath(path); }

bool containsHiddenPathSegment(const std::string& path) {
  if (path.empty()) return false;
  size_t segmentStart = (path.front() == '/') ? 1 : 0;
  while (segmentStart < path.length()) {
    const size_t segmentEnd = path.find('/', segmentStart);
    if (segmentStart < path.length() && path[segmentStart] == '.') {
      return true;
    }
    if (segmentEnd == std::string::npos) {
      break;
    }
    segmentStart = segmentEnd + 1;
  }
  return false;
}

void collectMetadataPathsRecursively(const std::string& dirPath, std::vector<std::string>& paths) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("FileBrowser", "Failed to scan directory metadata before delete: %s", dirPath.c_str());
    return;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string childPath = buildFullPath(dirPath, name);
    if (file.isDirectory()) {
      collectMetadataPathsRecursively(childPath, paths);
    } else if (hasFileMetadata(childPath)) {
      paths.push_back(childPath);
    }
    file.close();
  }
  dir.close();
}

std::string getFileName(std::string filename);
std::string getFileExtension(const std::string& filename);
}  // namespace

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, const Mode mode)
    : Activity("FileBrowser", renderer, mappedInput),
      mode(mode),
      basepath(initialPath.empty() ? "/" : std::move(initialPath)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

bool FileBrowserActivity::loadFilesIntoVector(size_t cap, bool& overflow) {
  files.clear();
  overflow = false;

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    return false;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return false;
  }

  const auto accept =
      mode == Mode::PickFirmware ? acceptFirmware : (mode == Mode::PickDirectory ? acceptDirectory : acceptCommon);

  files.reserve(std::min<size_t>(cap, INDEX_THRESHOLD));
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDir = file.isDirectory();
    if (!accept(fileNameBuffer.get(), isDir)) {
      file.close();
      continue;
    }

    if (files.size() >= cap) {
      overflow = true;
      file.close();
      break;
    }

    size_t entryLen = std::strlen(fileNameBuffer.get());
    if (isDir) {
      if (entryLen + 1 >= NAME_BUFFER_SIZE) {
        LOG_ERR("FileBrowser", "Skipping oversized directory entry: %s", fileNameBuffer.get());
        file.close();
        continue;
      }
      fileNameBuffer[entryLen++] = '/';
      fileNameBuffer[entryLen] = '\0';
    }

    if (!hasHeapForFileEntryAppend(files, entryLen)) {
      fileListMemoryLimited = true;
      LOG_ERR("FileBrowser", "Low heap while loading %s (entries=%u free=%u maxAlloc=%u)", basepath.c_str(),
              static_cast<unsigned>(files.size()), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      file.close();
      root.close();
      files.clear();
      return true;
    }

    files.emplace_back(fileNameBuffer.get());
    file.close();
  }
  if (FsHelpers::directoryIterationFailed(root)) {
    LOG_ERR("FileBrowser", "Directory listing failed before EOF: %s", basepath.c_str());
    fileListReadFailed = true;
    files.clear();
    root.close();
    return false;
  }
  root.close();
  return true;
}

void FileBrowserActivity::loadFiles() {
  RenderLock lock(*this);
  loadFilesLocked();
}

void FileBrowserActivity::loadFilesLocked() {
  usingIndex = false;
  clearIndexNameCache();
  fileListMemoryLimited = false;
  visibleBookFusionCache.clear();
  for (auto& cache : sortKeyCache) cache.clear();
  for (bool& ready : sortCacheReady) ready = false;
  fileListReadFailed = false;
  if (fileIndex) fileIndex->close();

  bool overflow = false;
  if (!loadFilesIntoVector(INDEX_THRESHOLD, overflow)) {
    return;
  }

  if (!overflow || fileListMemoryLimited) {
    sortFiles();
    return;
  }

  files.clear();
  files.shrink_to_fit();

  if (!fileIndex) fileIndex = makeUniqueNoThrow<FileIndex>();
  if (!indexEntry) indexEntry = makeUniqueNoThrow<FileIndex::Entry>();
  if (fileIndex && indexEntry) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

    const auto accept =
        mode == Mode::PickFirmware ? acceptFirmware : (mode == Mode::PickDirectory ? acceptDirectory : acceptCommon);
    if (fileIndex->open(basepath.c_str(), accept)) {
      usingIndex = true;
      return;
    }
    if (fileIndex->directoryReadFailed()) {
      LOG_ERR("FileBrowser", "index scan failed before EOF: %s", basepath.c_str());
      fileListReadFailed = true;
      files.clear();
      return;
    }
  } else {
    LOG_ERR("FileBrowser", "index alloc failed");
  }

  LOG_ERR("FileBrowser", "index unavailable for %s; showing first %u entries", basepath.c_str(),
          static_cast<unsigned>(INDEX_THRESHOLD));
  overflow = false;
  loadFilesIntoVector(INDEX_THRESHOLD, overflow);
  sortFiles();
}

size_t FileBrowserActivity::entryCount() const {
  return usingIndex && fileIndex ? fileIndex->totalCount() : files.size();
}

void FileBrowserActivity::clearIndexNameCache() {
  for (size_t i = 0; i < INDEX_ROW_CACHE_SIZE; i++) {
    indexCachedRows[i] = SIZE_MAX;
    indexCachedNames[i].clear();
  }
}

const char* FileBrowserActivity::entryNameAt(size_t row) {
  if (!usingIndex) {
    return files[row].c_str();
  }

  const size_t cacheSlot = row % INDEX_ROW_CACHE_SIZE;
  if (indexCachedRows[cacheSlot] != row) {
    if (!fileIndex || !indexEntry || !fileIndex->entryAt(row, *indexEntry)) {
      LOG_ERR("FileBrowser", "index read failed at row %u", static_cast<unsigned>(row));
      indexCachedRows[cacheSlot] = SIZE_MAX;
      indexCachedNames[cacheSlot] = "?";
      return indexCachedNames[cacheSlot].c_str();
    }

    indexCachedNames[cacheSlot].assign(indexEntry->name);
    if (indexEntry->isDir) indexCachedNames[cacheSlot] += '/';
    indexCachedRows[cacheSlot] = row;
  }
  return indexCachedNames[cacheSlot].c_str();
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "name buffer alloc failed (%u bytes)", static_cast<unsigned>(NAME_BUFFER_SIZE));
    fileListMemoryLimited = true;
    requestUpdate();
    return;
  }

  selectorIndex = 0;
  showFileSelection = true;

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
    requestUpdate();
    return;
  }

  const bool rootIsDirectory = root.isDirectory();
  root.close();

  if (!rootIsDirectory) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  uiReady = false;
  visibleRows = 1;
  topIndex = followListSelection(static_cast<int>(selectorIndex), 0, visibleRows, static_cast<int>(entryCount()));
  listNav.reset(static_cast<int>(selectorIndex));
  listNav.top = topIndex;
  listNav.visibleRows = visibleRows;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &FileBrowserActivity::onRowEvent, this);
  app.on(ACTION_SETTINGS, &FileBrowserActivity::onSettingsEvent, this);
  app.setScreen(&FileBrowserActivity::listScreen, this);
  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileNameBuffer.reset();
  fileIndex.reset();
  indexEntry.reset();
  clearIndexNameCache();
  usingIndex = false;
}

void FileBrowserActivity::promptDeleteFile(const std::string& fullPath, const std::string& entry) {
  auto handler = [this, fullPath](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }

    BookActions::clearFileMetadata(fullPath);
    if (!Storage.remove(fullPath.c_str())) {
      LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
      return;
    }
    SleepImageIndex::invalidateForPath(fullPath.c_str());

    if (isPinnedSleepFavorite(fullPath)) {
      unpinSleepFavorite();
    }

    {
      // The render task reads the file list and base path while it builds the
      // FreeInkUI rows. Do not replace their backing strings mid-build.
      RenderLock lock(*this);
      loadFilesLocked();
      if (entryCount() == 0) {
        selectorIndex = 0;
      } else if (selectorIndex >= entryCount()) {
        selectorIndex = entryCount() - 1;
      }
    }
    requestUpdate(true);
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
}

void FileBrowserActivity::promptDeleteDirectory(const std::string& fullPath, const std::string& entry,
                                                const bool ignoreInitialConfirmRelease) {
  const std::string dirPath = normalizeDirectoryPath(fullPath);
  auto handler = [this, dirPath](const ActivityResult& res) {
    longPressConfirmHandled = false;
    if (res.isCancelled) {
      return;
    }

    std::vector<std::string> metadataPaths;
    collectMetadataPathsRecursively(dirPath, metadataPaths);

    if (!Storage.removeDir(dirPath.c_str())) {
      LOG_ERR("FileBrowser", "Failed to delete directory: %s", dirPath.c_str());
      return;
    }
    SleepImageIndex::invalidateForPath(dirPath.c_str());

    for (const auto& metadataPath : metadataPaths) {
      BookActions::clearFileMetadata(metadataPath);
    }

    const std::string favoritePrefix = dirPath + "/";
    if (!APP_STATE.favoriteSleepImagePath.empty() && APP_STATE.favoriteSleepImagePath.rfind(favoritePrefix, 0) == 0) {
      unpinSleepFavorite();
    }
    if (isPreferredSleepFolder(dirPath)) {
      clearPreferredSleepFolder();
    }

    {
      // buildListScreen() dereferences the list entries on the render task.
      RenderLock lock(*this);
      loadFilesLocked();
      if (entryCount() == 0) {
        selectorIndex = 0;
      } else if (selectorIndex >= entryCount()) {
        selectorIndex = entryCount() - 1;
      }
    }
    requestUpdate(true);
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry, ignoreInitialConfirmRelease),
      handler);
}

void FileBrowserActivity::showDirectoryActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease) {
  const std::string fullPath = normalizeDirectoryPath(buildFullPath(basepath, entry));
  const bool useDefaultFolders = isDefaultSleepFolderPath(fullPath) || isPreferredSleepFolder(fullPath);
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.push_back({useDefaultFolders ? FileBrowserAction::ClearSleepFolder : FileBrowserAction::SetSleepFolder,
                   useDefaultFolders ? StrId::STR_USE_DEFAULT_SLEEP_FOLDERS : StrId::STR_SET_AS_SLEEP_FOLDER});
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, getFileName(entry),
                                                                     std::move(items), ignoreInitialConfirmRelease),
                         [this, fullPath, entry](const ActivityResult& result) {
                           longPressConfirmHandled = false;
                           if (result.isCancelled) {
                             return;
                           }

                           const auto action =
                               static_cast<FileBrowserAction>(std::get<FileBrowserActionResult>(result.data).action);
                           switch (action) {
                             case FileBrowserAction::Delete:
                               promptDeleteDirectory(fullPath, entry);
                               return;
                             case FileBrowserAction::SetSleepFolder:
                               setPreferredSleepFolder(fullPath);
                               return;
                             case FileBrowserAction::ClearSleepFolder:
                               clearPreferredSleepFolder();
                               return;
                             case FileBrowserAction::DeleteCache:
                             case FileBrowserAction::DeleteStats:
                             case FileBrowserAction::ToggleCompleted:
                             case FileBrowserAction::RemoveFromRecents:
                             case FileBrowserAction::PinFavorite:
                             case FileBrowserAction::UnpinFavorite:
                             case FileBrowserAction::ViewBookmarks:
                             case FileBrowserAction::ViewClippings:
                             case FileBrowserAction::DeleteBookmarks:
                             case FileBrowserAction::DeleteClippings:
                             case FileBrowserAction::EpubRenderMode:
                             case FileBrowserAction::ResetReaderSettings:
                             case FileBrowserAction::SendNearby:
                               return;
                           }
                         });
}

void FileBrowserActivity::pinSleepFavorite(const std::string& fullPath) {
  APP_STATE.favoriteSleepImagePath = fullPath;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to save favorite sleep image path: %s", fullPath.c_str());
    return;
  }
  LOG_INF("FileBrowser", "Pinned favorite sleep image: %s", fullPath.c_str());
  requestUpdate();
}

void FileBrowserActivity::unpinSleepFavorite() {
  if (APP_STATE.favoriteSleepImagePath.empty()) {
    return;
  }

  APP_STATE.favoriteSleepImagePath.clear();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to clear favorite sleep image path");
    return;
  }
  LOG_INF("FileBrowser", "Cleared favorite sleep image");
  requestUpdate();
}

bool FileBrowserActivity::isPinnedSleepFavorite(const std::string& fullPath) const {
  return APP_STATE.favoriteSleepImagePath == fullPath;
}

void FileBrowserActivity::setPreferredSleepFolder(const std::string& fullPath) {
  const std::string normalizedPath = normalizeDirectoryPath(fullPath);
  const std::string nextPath = isSleepFolderPreferencePath(normalizedPath) ? normalizedPath : std::string();
  if (APP_STATE.preferredSleepFolderPath == nextPath) {
    requestUpdate();
    return;
  }

  APP_STATE.preferredSleepFolderPath = nextPath;
  APP_STATE.clearRecentSleepHistory();
  SleepImageIndex::invalidate();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to save preferred sleep folder path: %s", normalizedPath.c_str());
    return;
  }
  LOG_INF("FileBrowser", "Preferred sleep folder set to: %s", nextPath.empty() ? "<default>" : nextPath.c_str());
  requestUpdate();
}

void FileBrowserActivity::clearPreferredSleepFolder() {
  if (APP_STATE.preferredSleepFolderPath.empty()) {
    requestUpdate();
    return;
  }

  APP_STATE.preferredSleepFolderPath.clear();
  APP_STATE.clearRecentSleepHistory();
  SleepImageIndex::invalidate();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to clear preferred sleep folder path");
    return;
  }
  LOG_INF("FileBrowser", "Cleared preferred sleep folder");
  requestUpdate();
}

bool FileBrowserActivity::isPreferredSleepFolder(const std::string& fullPath) const {
  return APP_STATE.preferredSleepFolderPath == normalizeDirectoryPath(fullPath);
}

bool FileBrowserActivity::isSleepFavoriteFolder(const std::string& fullPath) const {
  const std::string normalizedPath = normalizeDirectoryPath(fullPath);
  return isDefaultSleepFolderPath(normalizedPath) || isPreferredSleepFolder(normalizedPath);
}

void FileBrowserActivity::showFileActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease) {
  const std::string fullPath = buildFullPath(basepath, entry);
  std::vector<FileBrowserActionActivity::MenuItem> items = BookActions::buildBookActionItems(fullPath, false);

  if (BookActions::canSendNearby(fullPath)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  const bool canPinFavorite = isSleepFavoriteFolder(basepath) && isSleepImageFile(entry);
  if (canPinFavorite) {
    items.push_back(
        {isPinnedSleepFavorite(fullPath) ? FileBrowserAction::UnpinFavorite : FileBrowserAction::PinFavorite,
         isPinnedSleepFavorite(fullPath) ? StrId::STR_UNPIN_AS_FAVORITE : StrId::STR_PIN_AS_FAVORITE});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, getFileName(entry), std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, fullPath, entry](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          return;
        }

        const auto action = static_cast<FileBrowserAction>(std::get<FileBrowserActionResult>(result.data).action);
        switch (action) {
          case FileBrowserAction::BookInfo:
            startActivityForResult(
                std::make_unique<BookDetailsActivity>(renderer, mappedInput, fullPath, "", ""),
                [this](const ActivityResult&) { requestUpdate(); });
            return;
          case FileBrowserAction::SendNearby:
            activityManager.goToNearbyBookSend(fullPath, false);
            return;
          case FileBrowserAction::Delete:
            promptDeleteFile(fullPath, entry);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(std::make_unique<ConfirmationActivity>(
                                       renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE),
                                       getFileName(entry)),
                                   [this, fullPath](const ActivityResult& confirmation) {
                                     if (!confirmation.isCancelled) {
                                       if (!BookActions::clearBookCache(fullPath)) {
                                         LOG_ERR("FileBrowser", "Failed to clear book cache for: %s", fullPath.c_str());
                                       } else {
                                         BookActions::drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                                         delay(1000);
                                       }
                                     }
                                     requestUpdate();
                                   });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                       BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS),
                                                       getFileName(entry)),
                [this, fullPath](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(fullPath)) {
                      LOG_ERR("FileBrowser", "Failed to delete book stats for: %s", fullPath.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  requestUpdate();
                });
            return;
          case FileBrowserAction::ResetReaderSettings:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_RESET_BOOK_READER_SETTINGS),
                    getFileName(entry)),
                [this, fullPath](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::resetBookReaderSettings(fullPath)) {
                      LOG_ERR("FileBrowser", "Failed to reset reader settings for: %s", fullPath.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_READER_SETTINGS_RESET));
                      delay(1000);
                    }
                  }
                  requestUpdate();
                });
            return;
          case FileBrowserAction::ToggleCompleted:
            if (BookActions::toggleBookCompleted(fullPath, getFileName(entry), completedFeedbackIsFinished)) {
              pendingCompletedFeedback = true;
              completedFeedbackShowTime = millis();
            }
            {
              RenderLock lock(*this);
              loadFilesLocked();
              selectorIndex = entryCount() == 0 ? 0 : std::min(selectorIndex, entryCount() - 1);
            }
            requestUpdate(true);
            return;
          case FileBrowserAction::EpubRenderMode: {
            const uint8_t currentIndex =
                BookActions::epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(fullPath));
            startActivityForResult(
                std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "EpubRenderModeSelect",
                                                          StrId::STR_EPUB_RENDER_MODE,
                                                          BookActions::epubRenderModeOptions(), currentIndex),
                [this, fullPath](const ActivityResult& selectionResult) {
                  if (!selectionResult.isCancelled) {
                    const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
                    if (selection != nullptr &&
                        !EpubReaderActivity::saveBookRenderMode(
                            fullPath, BookActions::epubRenderModeForDisplayIndex(selection->index))) {
                      LOG_ERR("FileBrowser", "Failed to save render mode for: %s", fullPath.c_str());
                    }
                  }
                  requestUpdate();
                });
            return;
          }
          case FileBrowserAction::PinFavorite:
            if (FsHelpers::hasPngExtension(fullPath)) {
              startActivityForResult(
                  std::make_unique<ConfirmationActivity>(renderer, mappedInput, "", tr(STR_PIN_PNG_WARNING)),
                  [this, fullPath](const ActivityResult& confirmation) {
                    if (!confirmation.isCancelled) {
                      pinSleepFavorite(fullPath);
                    }
                  });
            } else {
              pinSleepFavorite(fullPath);
            }
            return;
          case FileBrowserAction::UnpinFavorite:
            unpinSleepFavorite();
            return;
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::RemoveFromRecents:
          case FileBrowserAction::ViewBookmarks:
          case FileBrowserAction::ViewClippings:
          case FileBrowserAction::DeleteBookmarks:
          case FileBrowserAction::DeleteClippings:
            return;
        }
      });
}

void FileBrowserActivity::toggleHiddenFiles() {
  const std::string currentEntry =
      (entryCount() > 0 && selectorIndex < entryCount()) ? entryNameAt(selectorIndex) : std::string();
  SETTINGS.showHiddenFiles = SETTINGS.showHiddenFiles ? 0 : 1;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to save showHiddenFiles=%u", SETTINGS.showHiddenFiles);
  }

  {
    RenderLock lock(*this);
    if (!SETTINGS.showHiddenFiles && containsHiddenPathSegment(basepath)) {
      basepath = "/";
    }

    loadFilesLocked();
    selectorIndex = currentEntry.empty() ? 0 : findEntry(currentEntry);
    if (entryCount() > 0 && selectorIndex >= entryCount()) {
      selectorIndex = entryCount() - 1;
    }
  }
  requestUpdate();
}

void FileBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FileBrowserActivity*>(user);
  std::string entry;
  {
    RenderLock lock(*self);
    if (event.value < 0) return;
    const size_t row = self->actionWindowFirst + static_cast<size_t>(event.value);
    if (row >= self->entryCount()) return;
    self->selectorIndex = row;
    entry = self->entryNameAt(self->selectorIndex);
  }
  if (event.longPress && self->mode == Mode::Books) {
    self->showFileSelection = true;
    self->app.clearTapFlash();
    if (entry.back() == '/') {
      self->showDirectoryActionMenu(entry);
    } else {
      self->showFileActionMenu(entry);
    }
    return;
  }
  // Activation navigates or opens; a lingering flash would gray an unrelated
  // row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void FileBrowserActivity::onSettingsEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<FileBrowserActivity*>(user);
  if (self->mode != Mode::Books || !self->mappedInput.hasTouchHardware()) return;
  self->app.clearTapFlash();
  self->openSettings();
}

void FileBrowserActivity::openSettings() {
  const std::string selectedEntry =
      entryCount() > 0 && selectorIndex < entryCount() ? entryNameAt(selectorIndex) : std::string();
  startActivityForResult(
      std::make_unique<SettingsActivity>(renderer, mappedInput, false, true, SettingsActivity::View::FileBrowser),
      [this, selectedEntry](const ActivityResult&) {
        {
          RenderLock lock(*this);
          if (!SETTINGS.showHiddenFiles && containsHiddenPathSegment(basepath)) {
            basepath = "/";
          }
          loadFilesLocked();
          selectorIndex = selectedEntry.empty() ? 0 : findEntry(selectedEntry);
          if (entryCount() > 0 && selectorIndex >= entryCount()) {
            selectorIndex = entryCount() - 1;
          }
          topIndex =
              followListSelection(static_cast<int>(selectorIndex), 0, visibleRows, static_cast<int>(entryCount()));
          listNav.reset(static_cast<int>(selectorIndex));
          listNav.top = topIndex;
          listNav.visibleRows = visibleRows;
        }
        requestUpdate();
      });
}

void FileBrowserActivity::activateSelected() {
  if (lockNextConfirmRelease) {
    lockNextConfirmRelease = false;
    return;
  }
  if (entryCount() == 0) return;

  const std::string entry = entryNameAt(selectorIndex);
  const bool isDirectory = (entry.back() == '/');

  // Firmware picker: select file -> return path; navigate into directories normally.
  if (mode == Mode::PickFirmware && !isDirectory) {
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  std::string fullPath;
  {
    // listScreen() reads basepath and file entry storage on the render task.
    // Keep their mutation together so it never sees freed row strings.
    RenderLock lock(*this);
    if (basepath.back() != '/') basepath += "/";
    if (isDirectory) {
      basepath += entry.substr(0, entry.length() - 1);
      loadFilesLocked();
      selectorIndex = 0;
      topIndex = 0;
      showFileSelection = true;
    } else {
      fullPath = basepath + entry;
    }
  }
  if (isDirectory) {
    requestUpdate();
  } else {
    onSelectBook(fullPath);
  }
}

void FileBrowserActivity::loop() {
  if (sortPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    navigateBack();
    return;
  }

  if (mode == Mode::Books && mappedInput.hasTouchHardware() && sortButtonRect.width > 0) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && tx >= sortButtonRect.x && tx < sortButtonRect.x + sortButtonRect.width &&
        ty >= sortButtonRect.y && ty < sortButtonRect.y + sortButtonRect.height) {
      openSortPopup();
      return;
    }
  }

  // Non-touch: PageBack/PageForward are otherwise unused on this screen (they're a
  // distinct button pair from Up/Down/Left/Right's list navigation), so either one
  // opens the sort popup -- no conflict with the existing long-press-Confirm context
  // menu or long-press-Back hidden-files toggle.
  if (mode == Mode::Books && !mappedInput.hasTouchHardware() &&
      (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
       mappedInput.wasPressed(MappedInputManager::Button::PageForward))) {
    openSortPopup();
    return;
  }
  if (pendingCompletedFeedback) {
    const bool timedOut = (millis() - completedFeedbackShowTime) >= COMPLETED_FEEDBACK_MS;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingCompletedFeedback = false;
      requestUpdate();
      return;
    }
  }

  // In directory-picker mode the fourth front button selects the folder shown
  // in the path band; Confirm continues to descend into the highlighted child.
  if (mode == Mode::PickDirectory && mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult result{FilePathResult{normalizeDirectoryPath(basepath)}};
    result.isCancelled = false;
    setResult(std::move(result));
    finish();
    return;
  }

  // Long press BACK (1s+) toggles hidden files (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && !longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && !lockLongPressBack) {
    longPressBackHandled = true;
    toggleHiddenFiles();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int listSize = static_cast<int>(entryCount());
  if (entryCount() > 0) {
    const std::string entry = entryNameAt(selectorIndex);
    const bool isDirectory = (entry.back() == '/');
    if (mode == Mode::Books && !longPressConfirmHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= GO_HOME_MS) {
      longPressConfirmHandled = true;
      if (isDirectory) {
        showDirectoryActionMenu(entry, true);
      } else {
        showFileActionMenu(entry, true);
      }
      return;
    }
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressConfirmHandled) {
      longPressConfirmHandled = false;
    } else {
      showFileSelection = true;
      activateSelected();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (longPressBackHandled) {
      longPressBackHandled = false;
      return;
    }
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      navigateBack();
    }
  }

  if (listSize <= 0) return;

  // Swipes scroll the viewport; the selection stays put and button navigation
  // pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    {
      RenderLock lock(*this);
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
      if (static_cast<size_t>(listSize) > MAX_VIRTUAL_LIST_ENTRIES) {
        const int nextTop = scrollListBy(topIndex, delta, visibleRows, listSize);
        moved = nextTop != topIndex;
        topIndex = nextTop;
      } else {
        listNav.selected = static_cast<int>(selectorIndex);
        listNav.top = topIndex;
        listNav.visibleRows = visibleRows;
        moved = listNav.scrollBy(delta, listSize);
        topIndex = listNav.top;
      }
    }
    if (moved) {
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, listSize](const int index) {
    {
      RenderLock lock(*this);
      selectorIndex = static_cast<size_t>(index);
      showFileSelection = true;
      if (static_cast<size_t>(listSize) > MAX_VIRTUAL_LIST_ENTRIES) {
        topIndex = followListSelection(index, topIndex, visibleRows, listSize);
      } else {
        listNav.selected = index;
        listNav.top = topIndex;
        listNav.visibleRows = visibleRows;
        listNav.follow(listSize);
        topIndex = listNav.top;
      }
    }
    requestUpdate();
  };
  const auto moveNext = [this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize));
  };
  const auto movePrevious = [this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize));
  };
  const auto pageNext = [this, listSize, &moveSelection] {
    int nextIndex;
    {
      RenderLock lock(*this);
      nextIndex = ButtonNavigator::nextPageIndex(
          static_cast<int>(selectorIndex), listSize,
          static_cast<size_t>(listSize) > MAX_VIRTUAL_LIST_ENTRIES ? visibleRows : listNav.pageRows());
    }
    moveSelection(nextIndex);
  };
  const auto pagePrevious = [this, listSize, &moveSelection] {
    int previousIndex;
    {
      RenderLock lock(*this);
      previousIndex = ButtonNavigator::previousPageIndex(
          static_cast<int>(selectorIndex), listSize,
          static_cast<size_t>(listSize) > MAX_VIRTUAL_LIST_ENTRIES ? visibleRows : listNav.pageRows());
    }
    moveSelection(previousIndex);
  };
  if (mode == Mode::PickDirectory) {
    // The front-right button selects this folder. Keep navigation on the side
    // buttons so the remaining front-left button is not a misleading one-way control.
    buttonNavigator.onRelease({MappedInputManager::Button::Down}, moveNext);
    buttonNavigator.onRelease({MappedInputManager::Button::Up}, movePrevious);
    buttonNavigator.onContinuous({MappedInputManager::Button::Down}, pageNext);
    buttonNavigator.onContinuous({MappedInputManager::Button::Up}, pagePrevious);
  } else {
    buttonNavigator.onNextRelease(moveNext);
    buttonNavigator.onPreviousRelease(movePrevious);
    buttonNavigator.onNextContinuous(pageNext);
    buttonNavigator.onPreviousContinuous(pagePrevious);
  }
}

void FileBrowserActivity::navigateBack() {
  if (basepath != "/") {
    {
      RenderLock lock(*this);
      const std::string oldPath = basepath;
      basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
      if (basepath.empty()) basepath = "/";
      loadFilesLocked();

      const std::string dirName = oldPath.substr(oldPath.find_last_of('/') + 1) + "/";
      selectorIndex = findEntry(dirName);
      showFileSelection = true;
      topIndex = followListSelection(static_cast<int>(selectorIndex), 0, visibleRows, static_cast<int>(entryCount()));
    }
    requestUpdate();
  } else if (mode != Mode::Books) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  } else {
    onGoHome();
  }
}

namespace {

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  if (pos == std::string::npos) {
    return "";
  }
  return filename.substr(pos);
}

// Case-folds a sort key so e.g. a stylised lowercase AO3 title ("// devotion")
// doesn't sort after every ordinarily-capitalized title just because uppercase
// letters precede lowercase ones in byte order -- a plain std::string compare
// otherwise clusters every such title at one end of the list instead of where
// a human reading it alphabetically would expect it.
std::string toLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Library-catalogue convention: "The Great Gatsby" sorts under G, not T. `lower`
// must already be lowercased. Only strips a genuine leading word (one followed
// by a space, with something left after it), so "A-Team" or "Anew" aren't
// touched, and a title that's just "The" isn't stripped down to nothing.
std::string stripLeadingArticleForSort(const std::string& lower) {
  static constexpr const char* kArticles[] = {"the ", "an ", "a "};
  for (const char* article : kArticles) {
    const size_t len = strlen(article);
    if (lower.size() > len && lower.compare(0, len, article) == 0) {
      return lower.substr(len);
    }
  }
  return lower;
}

}  // namespace

bool FileBrowserActivity::isBookFusionLinked(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return false;
  return BookFusionBookIdStore::hasBookId(path);
}

std::string FileBrowserActivity::computeSortKey(SortField field, const std::string& fullPath) {
  if (field == SortField::LastOpened) {
    const auto& recentBooks = RECENT_BOOKS.getBooks();
    for (size_t i = 0; i < recentBooks.size(); i++) {
      if (recentBooks[i].path == fullPath) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%04zu", i);
        return buf;
      }
    }
    return "";  // never opened -- sorts to the end, same as any other missing key
  }

  if (!FsHelpers::hasEpubExtension(fullPath)) return "";

  BookMetadataCache cache(Epub::cachePathForFilePath(fullPath, "/.crosspoint"));
  if (!cache.load()) return "";

  switch (field) {
    case SortField::Title:
      return stripLeadingArticleForSort(toLowerAscii(cache.coreMetadata.title));
    case SortField::Author:
      return toLowerAscii(cache.coreMetadata.author);
    case SortField::Status:
      return toLowerAscii(cache.coreMetadata.completionStatus);
    case SortField::Rating:
      return toLowerAscii(cache.coreMetadata.contentRating);
    case SortField::DateUpdated:
      return cache.coreMetadata.updatedDate;  // ISO YYYY-MM-DD -- lexical order is chronological
    case SortField::Chapters: {
      // Posted count is the left side of "N/M" (or "N/?" for an open-ended WIP);
      // zero-padded so lexical string comparison matches numeric order.
      const std::string& raw = cache.coreMetadata.chapters;
      const auto slash = raw.find('/');
      const std::string posted = slash != std::string::npos ? raw.substr(0, slash) : raw;
      if (posted.empty() || !std::all_of(posted.begin(), posted.end(), [](unsigned char c) { return isdigit(c); })) {
        return "";
      }
      char buf[8];
      snprintf(buf, sizeof(buf), "%05d", atoi(posted.c_str()));
      return buf;
    }
    default:
      return "";
  }
}

void FileBrowserActivity::ensureSortCache(SortField field, const std::vector<std::string>& nonDirEntries) {
  const int idx = static_cast<int>(field);
  if (sortCacheReady[idx]) return;
  auto& cache = sortKeyCache[idx];
  cache.clear();

  // LastOpened's key comes from RecentBooksStore, not BookMetadataCache -- there's
  // nothing to build for it.
  const bool needsMetadataCache = field != SortField::LastOpened;
  bool showingLoading = false;
  Rect popupRect;
  const int total = static_cast<int>(nonDirEntries.size());
  int processed = 0;

  // Same 80KB floor Ao3IndexActivity's own bulk book-processing pass checks before
  // it starts building: the OPF/TOC build pass has an unguarded allocation deep
  // inside that can abort the whole device on low heap (a known crash class in this
  // codebase), not just fail this one book gracefully. Checked per-book rather than
  // once up front -- each Epub is destroyed at the end of its own iteration (see
  // below), so heap recovers between books, and a folder that starts out fine can
  // still run low partway through a long batch of never-opened books.
  static constexpr uint32_t kMinFreeHeapForBuild = 80 * 1024;

  for (const auto& entry : nonDirEntries) {
    const std::string fullPath = buildFullPath(basepath, entry);
    // Skip the exists() check's own cost for books that already have a cache --
    // computeSortKey() below reads those directly and cheaply. Only pay for a full
    // Epub::load() (same lightweight, reader-free build RecentBooksGridActivity and
    // HomeActivity already use for cover-thumb generation) on the ones that need it.
    if (needsMetadataCache && FsHelpers::hasEpubExtension(fullPath) && ESP.getFreeHeap() >= kMinFreeHeapForBuild &&
        !BookMetadataCache::exists(Epub::cachePathForFilePath(fullPath, "/.crosspoint"))) {
      Epub epub(fullPath, "/.crosspoint");
      if (epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
        if (!showingLoading) {
          showingLoading = true;
          popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        }
        GUI.fillPopupProgress(renderer, popupRect, (processed * 100) / std::max(1, total));
      }
    }
    cache[entry] = computeSortKey(field, fullPath);
    processed++;
  }
  sortCacheReady[idx] = true;
}

void FileBrowserActivity::sortFiles() {
  std::vector<std::string> dirs;
  std::vector<std::string> nonDirs;
  dirs.reserve(files.size());
  nonDirs.reserve(files.size());
  for (auto& entry : files) {
    (entry.back() == '/' ? dirs : nonDirs).push_back(std::move(entry));
  }
  std::sort(dirs.begin(), dirs.end(), FsHelpers::naturalLess);

  const uint8_t fieldRaw = SETTINGS.fileBrowserSortField;
  if (mode != Mode::Books || fieldRaw >= SORT_FIELD_COUNT) {
    std::sort(nonDirs.begin(), nonDirs.end(), FsHelpers::naturalLess);
  } else {
    const auto field = static_cast<SortField>(fieldRaw);
    // nonDirs, not files -- files' contents were just std::move'd out into dirs/nonDirs
    // above, so by this point every entry in files is a moved-from empty string.
    ensureSortCache(field, nonDirs);
    const auto& keyCache = sortKeyCache[fieldRaw];
    const bool ascending = SETTINGS.fileBrowserSortAscending != 0;
    std::sort(nonDirs.begin(), nonDirs.end(), [&](const std::string& a, const std::string& b) {
      const auto itA = keyCache.find(a);
      const auto itB = keyCache.find(b);
      const std::string& keyA = itA != keyCache.end() ? itA->second : std::string();
      const std::string& keyB = itB != keyCache.end() ? itB->second : std::string();
      if (keyA.empty() != keyB.empty()) return keyB.empty();  // missing key always sorts to the end
      if (keyA.empty() || keyA == keyB) return FsHelpers::naturalLess(a, b);  // both missing, or tie -- stable fallback
      return ascending ? keyA < keyB : keyA > keyB;
    });
  }

  files.clear();
  files.reserve(dirs.size() + nonDirs.size());
  for (auto& d : dirs) files.push_back(std::move(d));
  for (auto& f : nonDirs) files.push_back(std::move(f));
}

void FileBrowserActivity::openSortPopup() {
  // tr(x) is a macro expanding to I18N.get(StrId::x) via token-pasting -- it can't take
  // a runtime variable, so this array/loop calls I18N.get() directly instead.
  static const StrId kFieldLabelIds[SORT_FIELD_COUNT] = {
      StrId::STR_TITLE,        StrId::STR_SORT_AUTHOR,       StrId::STR_SORT_STATUS, StrId::STR_SORT_RATING,
      StrId::STR_SORT_CHAPTERS, StrId::STR_SORT_DATE_UPDATED, StrId::STR_SORT_LAST_OPENED};
  std::vector<std::string> labels;
  labels.reserve(SORT_FIELD_COUNT);
  for (const StrId id : kFieldLabelIds) labels.emplace_back(I18N.get(id));

  const uint8_t fieldRaw = SETTINGS.fileBrowserSortField;
  const int activeField = fieldRaw < SORT_FIELD_COUNT ? static_cast<int>(fieldRaw) : -1;
  const bool ascending = SETTINGS.fileBrowserSortAscending != 0;

  sortPopup.show(
      StrId::STR_SORT, std::move(labels), activeField, ascending,
      [this](int field, bool asc) {
        SETTINGS.fileBrowserSortField = static_cast<uint8_t>(field);
        SETTINGS.fileBrowserSortAscending = asc ? 1 : 0;
        SETTINGS.saveToFile();
        RenderLock lock(*this);
        sortFiles();
        const std::string currentEntry = entryCount() > 0 ? entryNameAt(selectorIndex) : "";
        selectorIndex = currentEntry.empty() ? 0 : findEntry(currentEntry);
        topIndex = 0;
        requestUpdate();
      },
      [this] { requestUpdate(); });
}

void FileBrowserActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<FileBrowserActivity*>(user)->buildListScreen(screen);
}

void FileBrowserActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  if (mode == Mode::Books && mappedInput.hasTouchHardware()) {
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const auto backLayout = TouchHeaderBackButton::layout(header);
    const fui::Rect settingsRect{static_cast<int16_t>(header.x + header.width - backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.y),
                                 static_cast<int16_t>(backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.height)};
    fui::ButtonProps settings;
    settings.action = ACTION_SETTINGS;
    settings.styles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
    settings.minTouchSize = screen.theme().minTouchSize;
    screen.button(settings, settingsRect);
    const auto icon = fui::bitmapFromIcon(icon_sliders_horizontal_24);
    const int16_t iconX = static_cast<int16_t>(settingsRect.x + (settingsRect.width - icon.width) / 2);
    const int16_t iconY = static_cast<int16_t>(backLayout.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                                               (backLayout.iconRect.height - icon.height) / 2);
    screen.target().bitmap(fui::Rect{iconX, iconY, icon.width, icon.height}, icon, fui::BitmapMode::Center,
                           fui::Paint::solid(fui::Color::Black));
  }
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Full path band at the bottom: separator on top, left-truncated so the
  // deepest directory stays visible.
  {
    const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(pathLineHeight + metrics.verticalSpacing));
    screen.target().fill(fui::Rect{band.x, band.y, band.width, 3}, fui::Paint::solid(fui::Color::Black));
    const int pathY =
        band.y + metrics.verticalSpacing / 2 + (band.height - metrics.verticalSpacing / 2 - pathLineHeight) / 2;
    const int pathMaxWidth = band.width - metrics.contentSidePadding * 2;
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, band.x + metrics.contentSidePadding, pathY, pathDisplay);
  }

  const size_t totalEntries = entryCount();
  if (totalEntries == 0) {
    const char* emptyMessage =
        fileListMemoryLimited
            ? tr(STR_MEMORY_ERROR)
            : (fileListReadFailed ? tr(STR_ERROR_GENERAL_FAILURE)
                                  : (mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND)));
    screen.centeredText(emptyMessage, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  const bool twoLineRows = usesTwoLineFileBrowserRows();
  const auto rowType = twoLineRows ? UiListRowType::WithSubtitle : UiListRowType::SingleLine;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = twoLineRows ? 2 : 1;
  fui::Rect listRect = screen.body();
  // Reserve room for the persistent Sort edge tab (drawn in render(), after this
  // list) on every row, not just the one or two rows it happens to sit beside --
  // otherwise a row's own right edge / value column (e.g. the ".epub" extension)
  // could end up under the tab instead of next to it.
  if (mode == Mode::Books) {
    listRect.width = static_cast<int16_t>(std::max(0, listRect.width - kSortTabWidth));
  }
  const auto rows = configureUiList(props, screen.theme(), listRect, rowType);
  visibleRows = rows > 0 ? rows : 1;
  const bool usesVirtualList = totalEntries <= MAX_VIRTUAL_LIST_ENTRIES;
  if (usesVirtualList) {
    listNav.selected = static_cast<int>(selectorIndex);
    listNav.top = topIndex;
    listNav.visibleRows = visibleRows;
    listNav.syncToProps(listRect, props.rowHeight, props.rowGap, static_cast<int>(totalEntries), props);
    topIndex = listNav.top;
  } else {
    // FreeInkUI's virtual list carries both its count and selection in 16-bit
    // fields. Keep the proven local-window path for unusually large folders.
    topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(totalEntries));
  }
  const size_t drawCount = std::min<size_t>(visibleRows, totalEntries - static_cast<size_t>(topIndex));
  actionWindowFirst = usesVirtualList ? 0 : static_cast<size_t>(topIndex);

  // Only materialize the visible window. Large folders continue to use
  // FileIndex instead of duplicating every filename on the heap for UI rows.
  std::vector<std::string> names(drawCount);
  std::vector<std::string> values(drawCount);
  std::vector<fui::ListItem> items;
  items.reserve(drawCount);
  for (size_t i = 0; i < drawCount; i++) {
    const size_t entryIndex = static_cast<size_t>(topIndex) + i;
    const std::string entry = entryNameAt(entryIndex);
    names[i] = getFileName(entry);
    if (SETTINGS.hideFileExtension == 0) values[i] = getFileExtension(entry);
    const std::string fullPath = buildFullPath(basepath, entry);
    if ((entry.back() == '/' && isPreferredSleepFolder(fullPath)) || isPinnedSleepFavorite(fullPath)) {
      values[i] = values[i].empty() ? "*" : "* " + values[i];
    }
    bool isBookFusion = false;
    if (entry.back() != '/') {
      auto bfCached = visibleBookFusionCache.find(entryIndex);
      isBookFusion = bfCached != visibleBookFusionCache.end()
                         ? bfCached->second
                         : (visibleBookFusionCache[entryIndex] = isBookFusionLinked(fullPath));
    }
    fui::ListItem item;
    item.label = names[i].c_str();
    if (!values[i].empty()) item.value = values[i].c_str();
    // BookFusion-linked books display the BF mark in the row's icon slot instead
    // of the usual file-type icon (matching InsiderPhD's original file-browser badge).
    item.icon = listIconFor(isBookFusion ? UIIcon::BookFusion : UITheme::getFileIcon(entry), twoLineRows ? 32 : 24);
    item.actionValue = static_cast<int16_t>(usesVirtualList ? entryIndex : i);
    items.push_back(item);
  }

  props.items = items.data();
  props.count = static_cast<uint16_t>(usesVirtualList ? totalEntries : items.size());
  props.itemsWindowFirst = usesVirtualList ? static_cast<uint16_t>(topIndex) : 0;
  props.itemsWindowCount = usesVirtualList ? static_cast<uint16_t>(drawCount) : 0;
  if (!usesVirtualList) {
    props.selectedIndex =
        selectorIndex >= static_cast<size_t>(topIndex) && selectorIndex < static_cast<size_t>(topIndex) + drawCount
            ? static_cast<int16_t>(selectorIndex - static_cast<size_t>(topIndex))
            : -1;
    props.topIndex = 0;
    props.nav = nullptr;
  }
  props.action = ACTION_ROW;
  props.inputMask = mode == Mode::Books ? static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress)
                                        : fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;                                     // air between the extension and the row edge
  // A file extension is short, so do not sacrifice most of a two-line title
  // to visually balance it with the value column.
  props.balanceWrappedLabelWithValue = false;
  // The pinned FreeInkUI list computes its partial-row preview from
  // `top + visibleRows`, which is not reliable when wrapped rows consume more
  // than one line. Keep every rendered item contiguous until the SDK exposes
  // the consumed index.
  props.partialTrailingRow = false;
  screen.list(props);
  if (usesVirtualList) topIndex = listNav.top;
  fui::drawListScrollIndicator(screen.target(), listRect, totalEntries, visibleRows, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      mode == Mode::PickFirmware
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : (mode == Mode::PickDirectory
                 ? std::string(tr(STR_SELECT_RECEIVE_FOLDER))
                 : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1)));
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    // Sort now lives in the persistent edge tab drawn below (after app.render(), so
    // it overlays the list), not the header -- only the settings icon's own reserve
    // (drawn by buildListScreen()) needs accounting for here.
    if (mode == Mode::Books) {
      const auto backLayout = TouchHeaderBackButton::layout(header);
      const int settingsIconReserve = backLayout.iconRect.width + 8;
      TouchHeaderBackButton::draw(renderer, uiTarget, header, folderName.c_str(), false, settingsIconReserve);
    } else {
      TouchHeaderBackButton::draw(renderer, uiTarget, header, folderName.c_str(), false);
    }
  } else {
    GUI.drawHeader(renderer, header, folderName.c_str());
  }

  uiReady = false;
  for (int pass = 0; pass < 8; ++pass) {
    app.render();
    if (!listNav.consumeRebuildNeeded()) break;
  }
  uiReady = true;

  // Persistent "Sort" edge tab: visible on every device (not gated on touch hardware,
  // unlike the old header-only button this replaces), so button-only readers can see
  // sorting exists at all -- previously only reachable via an undiscoverable PageBack/
  // PageForward binding (see loop()), with zero on-screen hint. Drawn after app.render()
  // so it overlays the list, matching InsiderPhD's fork's fixed vertical edge-tab
  // convention -- but buildListScreen() also reserves kSortTabWidth of list width on
  // every row (not just the ones the tab happens to sit beside), so the opaque overlay
  // never actually needs to cover live row content; it only ever paints over the blank
  // margin reserved for it. Touch-only devices still use sortButtonRect as the tap
  // target; button-only devices get the same visual with no touch handling attached
  // (loop() only checks it under hasTouchHardware()).
  if (mode == Mode::Books) {
    const int sortLabelLen = renderer.getTextWidth(SMALL_FONT_ID, tr(STR_SORT));
    const int sortTabHeight = sortLabelLen + kSortTabPadding * 2;
    const int sortTabX = pageWidth - kSortTabWidth;
    const int sortTabY = header.y + header.height + kSortTabTopMargin;
    sortButtonRect = Rect{sortTabX, sortTabY, kSortTabWidth, sortTabHeight};
    // Flush against the right edge and open there -- rounded only on the left,
    // same technique (and the same radius) the bottom button hints use to sit
    // open against the screen's bottom edge: the two corners toward the edge
    // stay square and that whole edge segment goes undrawn, rather than a fully
    // enclosed box floating away from the edge.
    renderer.fillRoundedRect(sortTabX, sortTabY, kSortTabWidth, sortTabHeight, kSortTabCornerRadius,
                             /*roundTopLeft=*/true, /*roundTopRight=*/false, /*roundBottomLeft=*/true,
                             /*roundBottomRight=*/false, Color::White);
    renderer.drawRoundedRect(sortTabX, sortTabY, kSortTabWidth, sortTabHeight, 1, kSortTabCornerRadius,
                             /*roundTopLeft=*/true, /*roundTopRight=*/false, /*roundBottomLeft=*/true,
                             /*roundBottomRight=*/false, /*state=*/true);
    const int textX = sortTabX + (kSortTabWidth - renderer.getTextHeight(SMALL_FONT_ID)) / 2;
    const int textY = sortTabY + (sortTabHeight + sortLabelLen) / 2;
    renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, tr(STR_SORT));
  } else {
    sortButtonRect = Rect{0, 0, 0, 0};
  }

  const size_t visibleEntries = entryCount();
  const auto backLabel = (basepath == "/") ? (mode == Mode::Books ? mappedInput.withBackArrow(tr(STR_HOME))
                                                                  : mappedInput.withBackArrow(tr(STR_BACK)))
                                           : mappedInput.withBackArrow(tr(STR_BACK));
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile =
      mode == Mode::PickFirmware && visibleEntries > 0 && std::string(entryNameAt(selectorIndex)).back() != '/';
  const char* confirmLabel = visibleEntries == 0 ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(
      backLabel, confirmLabel, visibleEntries == 0 || mode == Mode::PickDirectory ? "" : tr(STR_DIR_UP),
      mode == Mode::PickDirectory ? tr(STR_SELECT) : (visibleEntries == 0 ? "" : tr(STR_DIR_DOWN)));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (!mappedInput.hasTouch() && mode == Mode::Books && basepath == "/") {
    const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int bandY = renderer.getScreenHeight() - metrics.buttonHintsHeight - pathLineHeight - metrics.verticalSpacing;
    const int pathY = bandY + metrics.verticalSpacing / 2 +
                      (pathLineHeight + metrics.verticalSpacing - metrics.verticalSpacing / 2 - pathLineHeight) / 2;
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    const int usedPathWidth = renderer.getTextWidth(SMALL_FONT_ID, basepath.c_str());
    const int hintMaxWidth = pathMaxWidth - usedPathWidth - ROOT_HINT_GAP;
    const auto hint = renderer.truncatedText(SMALL_FONT_ID, tr(STR_TOGGLE_HIDDEN_FILES_HINT), hintMaxWidth);
    const int hintWidth = renderer.getTextWidth(SMALL_FONT_ID, hint.c_str());
    renderer.drawText(SMALL_FONT_ID, pageWidth - metrics.contentSidePadding - hintWidth, pathY, hint.c_str());
  }

  if (pendingCompletedFeedback) {
    GUI.drawPopup(renderer, completedFeedbackIsFinished ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
  }

  if (sortPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) {
  if (usingIndex && fileIndex) {
    std::string raw = name;
    if (!raw.empty() && raw.back() == '/') raw.pop_back();
    const size_t row = fileIndex->findRowByName(raw.c_str());
    return row == SIZE_MAX ? 0 : row;
  }

  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
