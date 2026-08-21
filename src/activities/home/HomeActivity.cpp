#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_random.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../reader/BookReadingStats.h"
#include "../reader/BookStatsActivity.h"
#include "../reader/EpubReaderUtils.h"
#include "../settings/BookFusionSettingsActivity.h"
#include "Ao3LibraryActivity.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "SavedItemsHomeActivity.h"
#include "companion/CompanionRenderer.h"
#include "companion/CompanionState.h"
#include "companion/CompanionTracker.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t CAROUSEL_CACHE_MAGIC = 0x43434152;  // "CCAR"
// Cached frames include all Home visuals, including the menu icons. Bump this
// whenever their rendering changes so stale snapshots are rebuilt after OTA.
constexpr uint16_t CAROUSEL_CACHE_VERSION = 8;  // v8: AO3 Library icon fixed to Library (was Book), matching xAO3
constexpr char CAROUSEL_CACHE_PATH[] = "/.crosspoint/home_carousel_cache.bin";
constexpr char CAROUSEL_CACHE_TMP_PATH[] = "/.crosspoint/home_carousel_cache.tmp";
constexpr uint32_t CAROUSEL_FRAME_MIN_FREE_AFTER_ALLOC = 64U * 1024U;
constexpr uint32_t CAROUSEL_FRAME_MIN_MAX_ALLOC_AFTER_ALLOC = 24U * 1024U;
constexpr unsigned long HOME_BOOK_SWAP_LONG_PRESS_MS = 1000;
constexpr int HOME_BOOK_SWAP_RECENT_COUNT = 2;

enum class HomeMenuAction {
  BrowseFiles,
  ContinueReading,
  RecentBooks,
  OpdsBrowser,
  Ao3Library,
  BookFusion,
  ReadingStats,
  Bookmarks,
  FileTransfer,
  Settings,
};

struct HomeMenuEntry {
  const char* label;
  UIIcon icon;
  HomeMenuAction action;
};

struct HomeMenuEntries {
  static constexpr int kCapacity = 9;
  std::array<HomeMenuEntry, kCapacity> entries{};
  int count = 0;

  void push(const HomeMenuEntry& entry) {
    if (count >= kCapacity) return;
    entries[count++] = entry;
  }

  int size() const { return count; }

  const HomeMenuEntry& operator[](int index) const { return entries[index]; }
};

bool containsPoint(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

struct CarouselCacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t frameCount;
  uint32_t frameBufferSize;
  uint64_t keyHash;
  uint16_t screenWidth;
  uint16_t screenHeight;
  uint16_t centerCoverW;
  uint16_t centerCoverH;
  uint16_t sideCoverW;
  uint16_t sideCoverH;
};

uint64_t fnvHash64(const std::string& s) {
  uint64_t hash = 14695981039346656037ull;
  for (char c : s) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool hasAnyBookStats(const BookReadingStats& stats) {
  return stats.sessionCount > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 || stats.isCompleted ||
         stats.startDate.isValid() || stats.finishedDate.isValid();
}

bool hasAnyGlobalStats(const GlobalReadingStats& stats) {
  return stats.totalSessions > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 ||
         stats.completedBooks > 0 || stats.displayLongestReadingStreak() > 0;
}

bool hasHeapForCarouselFrameCache() {
  return ESP.getFreeHeap() >= CAROUSEL_FRAME_MIN_FREE_AFTER_ALLOC &&
         ESP.getMaxAllocHeap() >= CAROUSEL_FRAME_MIN_MAX_ALLOC_AFTER_ALLOC;
}

void appendHashedFileStateToKey(std::string& key, const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("HOME", path, file)) {
    key += "missing";
    key += '\0';
    return;
  }

  uint64_t hash = 14695981039346656037ull;
  size_t totalBytes = 0;
  uint8_t buffer[64];
  while (true) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;
    totalBytes += static_cast<size_t>(bytesRead);
    for (int i = 0; i < bytesRead; ++i) {
      hash ^= buffer[i];
      hash *= 1099511628211ull;
    }
  }
  file.close();

  char digest[48];
  snprintf(digest, sizeof(digest), "%zu:%" PRIu64, totalBytes, static_cast<uint64_t>(hash));
  key += digest;
  key += '\0';
}

std::string getRecentBookCachePath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub::cachePathForFilePath(book.path, "/.crosspoint");
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return "/.crosspoint/txt_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  return "";
}

BookReadingStats loadRecentBookStats(const RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path) && !FsHelpers::hasXtcExtension(book.path)) {
    return BookReadingStats{};
  }

  const std::string cachePath = getRecentBookCachePath(book);
  return BookReadingStats::load(cachePath);
}

bool loadEpubHighlightedContext(const RecentBook& book, const bool loadProgress, const bool loadChapterTitle,
                                float* progressPercent, std::string* chapterTitle) {
  if (!FsHelpers::hasEpubExtension(book.path) || (!loadProgress && !loadChapterTitle)) {
    return false;
  }

  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return false;
  }

  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgress(epub, progress, "HOME")) {
    return false;
  }

  if (loadProgress && progressPercent) {
    if (progress.hasPageCount && progress.pageCount > 0) {
      const float chapterProgress =
          static_cast<float>(progress.pageNumber + 1) / static_cast<float>(progress.pageCount);
      *progressPercent =
          std::clamp(epub.calculateProgress(progress.spineIndex, chapterProgress) * 100.0f, 0.0f, 100.0f);
    } else {
      *progressPercent = -1.0f;
    }
  }

  if (loadChapterTitle && chapterTitle) {
    chapterTitle->clear();
    const auto spineItem = epub.getSpineItem(progress.spineIndex);
    if (spineItem.tocIndex >= 0) {
      *chapterTitle = epub.getTocItem(spineItem.tocIndex).title;
    }
  }

  return true;
}

void updateRecentBookCover(const RecentBook& book) {
  if (!RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath, book.coverState)) {
    LOG_ERR("HOME", "failed to update recent book metadata: %s", book.path.c_str());
  }
}

void markCoverMissing(RecentBook& book) {
  book.coverBmpPath.clear();
  book.coverState = RecentBook::CoverState::Missing;
  updateRecentBookCover(book);
}

bool hasThumbnailPlaceholder(const std::string& coverBmpPath) {
  return coverBmpPath.find("[WIDTH]") != std::string::npos || coverBmpPath.find("[HEIGHT]") != std::string::npos;
}

std::string getReusableCoverPath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return Xtc(book.path, "/.crosspoint").getThumbBmpPath();
  }
  return book.coverBmpPath;
}

bool ensureReusableCoverPath(RecentBook& book) {
  if (book.coverState == RecentBook::CoverState::Missing || hasThumbnailPlaceholder(book.coverBmpPath)) {
    return false;
  }

  const std::string reusablePath = getReusableCoverPath(book);
  if (reusablePath.empty() || reusablePath == book.coverBmpPath) {
    return false;
  }

  book.coverBmpPath = reusablePath;
  updateRecentBookCover(book);
  return true;
}

const char* savedItemsLabel(bool hasBookmarks, bool hasClippings) {
  if (hasBookmarks && hasClippings) return tr(STR_BOOKMARKS_AND_CLIPPINGS);
  if (hasClippings) return tr(STR_CLIPPINGS);
  return tr(STR_BOOKMARKS);
}

void appendHomeMenuItems(HomeMenuEntries& items, bool hasOpdsServers, bool hasAo3Library, bool hasBookFusion,
                         bool hasReadingStats, bool hasBookmarks, bool hasClippings) {
  items.push({tr(STR_BROWSE_FILES), Folder, HomeMenuAction::BrowseFiles});
  items.push({tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks});

  if (hasOpdsServers) {
    items.push({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasAo3Library) {
    items.push({tr(STR_AO3_LIBRARY), Library, HomeMenuAction::Ao3Library});
  }
  if (hasBookFusion) {
    items.push({tr(STR_BF_SYNC), BookFusion, HomeMenuAction::BookFusion});
  }
  if (hasReadingStats) {
    items.push({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }
  if (hasBookmarks || hasClippings) {
    items.push({savedItemsLabel(hasBookmarks, hasClippings), BookmarkIcon, HomeMenuAction::Bookmarks});
  }

  items.push({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  items.push({tr(STR_SETTINGS_TITLE), Settings, HomeMenuAction::Settings});
}

HomeMenuEntries buildHomeMenuItems(bool hasOpdsServers, bool hasAo3Library, bool hasBookFusion, bool hasReadingStats,
                                   bool hasBookmarks, bool hasClippings) {
  HomeMenuEntries items;
  appendHomeMenuItems(items, hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks,
                      hasClippings);
  return items;
}

HomeMenuEntries buildMinimalMenuItems(bool hasOpdsServers, bool hasAo3Library, bool hasBookFusion,
                                      bool hasReadingStats, bool hasBookmarks, bool hasClippings) {
  HomeMenuEntries items;
  items.push({tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks});

  if (hasOpdsServers) {
    items.push({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasAo3Library) {
    items.push({tr(STR_AO3_LIBRARY), Library, HomeMenuAction::Ao3Library});
  }
  if (hasBookFusion) {
    items.push({tr(STR_BF_SYNC), BookFusion, HomeMenuAction::BookFusion});
  }
  if (hasBookmarks || hasClippings) {
    items.push({savedItemsLabel(hasBookmarks, hasClippings), BookmarkIcon, HomeMenuAction::Bookmarks});
  }
  if (hasReadingStats) {
    items.push({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }

  items.push({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  return items;
}

HomeMenuEntries buildSelectableHomeMenuItems(bool hasOpdsServers, bool hasAo3Library, bool hasBookFusion,
                                             bool hasReadingStats, bool hasBookmarks, bool hasClippings,
                                             bool includeContinueReading) {
  HomeMenuEntries items;
  if (includeContinueReading) {
    items.push({tr(STR_CONTINUE_READING), Book, HomeMenuAction::ContinueReading});
  }
  appendHomeMenuItems(items, hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks,
                      hasClippings);
  return items;
}

HomeMenuAction homeActionForInitialMenuItem(HomeMenuItem item) {
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      return HomeMenuAction::BrowseFiles;
    case HomeMenuItem::RECENTS:
      return HomeMenuAction::RecentBooks;
    case HomeMenuItem::OPDS_BROWSER:
      return HomeMenuAction::OpdsBrowser;
    case HomeMenuItem::FILE_TRANSFER:
      return HomeMenuAction::FileTransfer;
    case HomeMenuItem::SETTINGS_MENU:
      return HomeMenuAction::Settings;
    case HomeMenuItem::NONE:
    default:
      return HomeMenuAction::ContinueReading;
  }
}

int findMenuActionIndex(const HomeMenuEntries& items, HomeMenuAction action) {
  for (int i = 0; i < items.size(); ++i) {
    if (items[i].action == action) {
      return i;
    }
  }
  return -1;
}

bool isMinimalTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::MINIMAL;
}

bool isDashboardTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::DASHBOARD;
}

bool usesMinimalHomeInteraction() { return isMinimalTheme() || isDashboardTheme(); }

bool showMinimalHomeButtonHints(const MappedInputManager& mappedInput) { return !mappedInput.hasTouch(); }

bool isAnyFrontButtonPressed(const MappedInputManager& mappedInput) {
  return mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}

int minimalHomeNavCount(const bool hasCurrentBook) { return hasCurrentBook ? 4 : 3; }

int minimalHomeCoverWidth(int coverHeight) {
  (void)coverHeight;
  return MinimalMetrics::homeCoverImageWidth;
}

int minimalHomeCoverHeight(int coverHeight) {
  (void)coverHeight;
  return MinimalMetrics::homeCoverImageHeight;
}

std::string minimalHomeCoverPath(const RecentBook& book, int coverHeight) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint")
        .getAdaptiveThumbBmpPath(minimalHomeCoverWidth(coverHeight), minimalHomeCoverHeight(coverHeight));
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, minimalHomeCoverWidth(coverHeight),
                                    minimalHomeCoverHeight(coverHeight));
}

int dashboardHomeCoverWidth(int coverHeight) {
  (void)coverHeight;
  return DashboardMetrics::homeCoverImageWidth;
}

int dashboardHomeCoverHeight(int coverHeight) {
  (void)coverHeight;
  return DashboardMetrics::homeCoverImageHeight;
}

std::string dashboardHomeCoverPath(const RecentBook& book, int coverHeight) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint")
        .getAdaptiveThumbBmpPath(dashboardHomeCoverWidth(coverHeight), dashboardHomeCoverHeight(coverHeight));
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, dashboardHomeCoverWidth(coverHeight),
                                    dashboardHomeCoverHeight(coverHeight));
}

void appendCarouselCoverStateToKey(std::string& key, const RecentBook& book) {
  key += book.path;
  key += '\0';
  key += book.coverBmpPath;
  key += '\0';

  if (book.coverBmpPath.empty()) {
    key += "0:0";
    key += '\0';
    return;
  }

  const std::string centerPath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH);
  const std::string sidePath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
  key += Storage.exists(centerPath.c_str()) ? '1' : '0';
  key += ':';
  key += Storage.exists(sidePath.c_str()) ? '1' : '0';
  key += '\0';

  const std::string cachePath = getRecentBookCachePath(book);
  if (!cachePath.empty()) {
    appendHashedFileStateToKey(key, cachePath + "/progress.bin");
    if (FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasXtcExtension(book.path)) {
      appendHashedFileStateToKey(key, cachePath + "/stats_v5.bin");
    }
  } else {
    key += "no-cache-path";
    key += '\0';
  }
}

void appendSyncedStatsStateToKey(std::string& key) {
  FsFile dir = Storage.open("/.crosspoint/synced_stats");
  if (!dir) {
    key += "no-synced-stats";
    key += '\0';
    return;
  }

  if (!dir.isDirectory()) {
    dir.close();
    key += "synced-stats-not-dir";
    key += '\0';
    return;
  }

  char name[128];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    if (!isDirectory && nameLen > 0) {
      key += name;
      key += '\0';
      file.close();
      appendHashedFileStateToKey(key, std::string("/.crosspoint/synced_stats/") + name);
      continue;
    }
    file.close();
  }
  dir.close();
}

void appendCarouselMenuStateToKey(std::string& key, const bool hasOpdsServers, const bool hasAo3Library,
                                  const bool hasBookFusion, const bool hasReadingStats, const bool hasBookmarks,
                                  const bool hasClippings) {
  key += hasOpdsServers ? "opds:1" : "opds:0";
  key += '\0';
  key += hasAo3Library ? "ao3:1" : "ao3:0";
  key += '\0';
  key += hasBookFusion ? "bf:1" : "bf:0";
  key += '\0';
  key += hasReadingStats ? "stats:1" : "stats:0";
  key += '\0';
  key += hasBookmarks ? "bookmarks:1" : "bookmarks:0";
  key += '\0';
  key += hasClippings ? "clippings:1" : "clippings:0";
  key += '\0';
}

void buildCarouselCacheKey(const std::vector<RecentBook>& recentBooks, const bool hasOpdsServers,
                           const bool hasAo3Library, const bool hasBookFusion, const bool hasReadingStats,
                           const bool hasBookmarks, const bool hasClippings, std::string& key, uint64_t& keyHash) {
  key.clear();
  key.reserve(512);
  // The carousel cache stores the bottom icon row too, so menu visibility must
  // be part of the key alongside book covers/progress.
  appendCarouselMenuStateToKey(key, hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks,
                               hasClippings);
  for (const auto& book : recentBooks) {
    appendCarouselCoverStateToKey(key, book);
  }
  appendHashedFileStateToKey(key, "/.crosspoint/global_stats.bin");
  appendSyncedStatsStateToKey(key);
  keyHash = fnvHash64(key);
}

bool isCarouselCacheHeaderValid(const CarouselCacheHeader& header, uint64_t cacheKeyHash, int bookCount,
                                const GfxRenderer& renderer) {
  return header.magic == CAROUSEL_CACHE_MAGIC && header.version == CAROUSEL_CACHE_VERSION &&
         header.keyHash == cacheKeyHash && header.frameCount == bookCount &&
         header.frameBufferSize == renderer.getBufferSize() && header.screenWidth == renderer.getScreenWidth() &&
         header.screenHeight == renderer.getScreenHeight() && header.centerCoverW == LyraCarouselTheme::kCenterThumbW &&
         header.centerCoverH == LyraCarouselTheme::kCenterThumbH &&
         header.sideCoverW == LyraCarouselTheme::kSideCoverW && header.sideCoverH == LyraCarouselTheme::kSideCoverH;
}

bool readCarouselCacheHeader(FsFile& file, CarouselCacheHeader& header) {
  CarouselCacheHeader readHeader{};
  if (!serialization::tryReadPod(file, readHeader)) {
    return false;
  }
  header = readHeader;
  return true;
}

bool hasValidCarouselDiskCache(const std::vector<RecentBook>& recentBooks, const GfxRenderer& renderer,
                               const bool hasOpdsServers, const bool hasAo3Library, const bool hasBookFusion,
                               const bool hasReadingStats, const bool hasBookmarks, const bool hasClippings) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount <= 0) return false;

  std::string cacheKey;
  uint64_t cacheKeyHash = 0;
  buildCarouselCacheKey(recentBooks, hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks,
                        hasClippings, cacheKey, cacheKeyHash);

  FsFile cacheFile;
  if (!Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, cacheFile)) {
    return false;
  }

  CarouselCacheHeader header{};
  const bool readOk = readCarouselCacheHeader(cacheFile, header);
  cacheFile.close();
  return readOk && isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer);
}

int getVisibleRecentBookCount(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::min(static_cast<int>(recentBooks.size()), metrics.homeRecentBooksCount);
}

int getHomeMenuSelectionOffset(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.homeContinueReadingInMenu ? 0 : getVisibleRecentBookCount(recentBooks);
}

}  // namespace

// ---------------------------------------------------------------------------
// Static carousel frame cache — survives HomeActivity re-creation so that
// returning to home (e.g. after settings) doesn't re-read covers from SD.
// Freed explicitly in onSelectBook() before entering the reader.
// ---------------------------------------------------------------------------
namespace {
class CarouselCache {
 public:
  uint8_t* frames[HomeActivity::kCarouselFrameCount] = {};
  int frameBookIdx[HomeActivity::kCarouselFrameCount] = {-1};
  int frameCount = 0;
  int lastCenterIdx = -1;
  std::string key;
  uint64_t keyHash = 0;

  int findFrameSlot(int bookIdx) const {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frameBookIdx[i] == bookIdx && frames[i] != nullptr) return i;
    }
    return -1;
  }

  void invalidate() {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frames[i]) {
        free(frames[i]);
        frames[i] = nullptr;
      }
      frameBookIdx[i] = -1;
    }
    frameCount = 0;
    lastCenterIdx = -1;
    key.clear();
    keyHash = 0;
  }
};

CarouselCache gCarouselCache;

// One-shot: set by the boot path, consumed by the first home paint.
bool panelHoldsRetainedFrame = false;
}  // namespace

void HomeActivity::notePanelHoldsRetainedFrame() { panelHoldsRetainedFrame = true; }

static_assert(HomeActivity::kMaxCachedBooks >= LyraCarouselMetrics::values.homeRecentBooksCount,
              "kMaxCachedBooks must cover all carousel slots");

int HomeActivity::getMenuItemCount() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count += getVisibleRecentBookCount();
  } else if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count++;  // Continue Reading menu item
  }
  if (hasOpdsServers) {
    count++;
  }
  if (hasAo3Library) {
    count++;
  }
  if (hasBookFusion) {
    count++;
  }
  if (hasReadingStats) {
    count++;
  }
  if (hasBookmarks || hasClippings) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& storedBook : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    RecentBook book = storedBook;
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    ensureReusableCoverPath(book);
    recentBooks.push_back(book);
  }
}

void HomeActivity::loadAllBookStats() {
  const auto start = millis();
  const int count = std::min(static_cast<int>(recentBooks.size()), kMaxCachedBooks);
  for (int i = 0; i < count; ++i) {
    cachedBookStats[i] = loadRecentBookStats(recentBooks[i]);
    cachedBookProgress[i] = RecentBookProgress::loadPercent(recentBooks[i]);
  }
  bookStatsCached = true;
  LOG_DBG("HOME", "carousel: cached stats/progress for %d book(s) in %lums", count, millis() - start);
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  // Thumbnail generation may need a 32 KB contiguous inflate buffer. The Home
  // cover snapshot is only a redraw cache, so release it before ZIP work.
  if (coverBuffer) {
    freeCoverBuffer();
    coverRendered = false;
  }

  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  auto showLoadingProgress = [&](const int value) {
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, popupRect, std::clamp(value, 0, 100));
    renderer.displayBuffer();
  };

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const bool isMinimal = isMinimalTheme();
  const bool isDashboard = isDashboardTheme();
  const size_t recentBookCount = recentBooks.size();
  // Home only loads kMaxCachedBooks recents; fixed storage avoids an aborting std::vector allocation on low heap.
  std::array<char, kMaxCachedBooks> bookUpdated{};
  const int progressIncrement = 90 / static_cast<int>(std::max<size_t>(1, recentBookCount));

  int progress = 0;
  for (size_t bookIdx = 0; bookIdx < recentBooks.size(); ++bookIdx) {
    RecentBook& book = recentBooks[bookIdx];
    if (!Storage.exists(book.path.c_str())) {
      progress++;
      continue;
    }
    ensureReusableCoverPath(book);
    if (!book.coverBmpPath.empty()) {
      if (isCarouselTheme) {
        // For carousel: generate exact-size thumbnails for the center image rect and side slots.
        // Load the source image once even when both sizes are missing.
        const std::string centerPath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW,
                                                                  LyraCarouselTheme::kCenterThumbH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                LyraCarouselTheme::kSideCoverH);
        const bool centerMissing = !Storage.exists(centerPath.c_str());
        const bool sideMissing = !Storage.exists(sidePath.c_str());

        if (centerMissing || sideMissing) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            showLoadingProgress(10 + progress * progressIncrement);
            if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
              LOG_ERR("HOME", "carousel: failed to load EPUB cache for thumb generation: %s", book.path.c_str());
              coverRendered = false;
              requestUpdate();
              progress++;
              continue;
            }
            bool success = true;
            if (centerMissing)
              success = epub.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH,
                                              &renderer, SETTINGS.getReaderFontId()) &&
                        success;
            if (sideMissing)
              success = epub.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH, &renderer,
                                              SETTINGS.getReaderFontId()) &&
                        success;
            if (!success) {
              if (!epub.hasCoverImage()) markCoverMissing(book);
            } else if (bookIdx < bookUpdated.size()) {
              bookUpdated[bookIdx] = true;
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              showLoadingProgress(10 + progress * progressIncrement);
              bool success = true;
              if (centerMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) && success;
              if (sideMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
              if (success) {
                if (bookIdx < bookUpdated.size()) bookUpdated[bookIdx] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      } else {
        // Non-carousel: generate the active theme's thumbnail size.
        const bool supportsExactHomeThumb =
            FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasXtcExtension(book.path);
        const bool useDashboardThumb = isDashboard && supportsExactHomeThumb;
        const bool useMinimalThumb = isMinimal && supportsExactHomeThumb;
        const bool useExactHomeThumb = useDashboardThumb || useMinimalThumb;
        const std::string coverPath =
            useDashboardThumb ? dashboardHomeCoverPath(book, coverHeight)
                              : (useMinimalThumb ? minimalHomeCoverPath(book, coverHeight)
                                                 : UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight));
        if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            showLoadingProgress(10 + progress * progressIncrement);
            if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
              LOG_ERR("HOME", "failed to load EPUB cache for thumb generation: %s", book.path.c_str());
              coverRendered = false;
              requestUpdate();
              progress++;
              continue;
            }
            const bool success =
                useDashboardThumb
                    ? epub.generateAdaptiveThumbBmp(dashboardHomeCoverWidth(coverHeight),
                                                    dashboardHomeCoverHeight(coverHeight), &renderer,
                                                    SETTINGS.getReaderFontId())
                    : (useExactHomeThumb
                           ? epub.generateAdaptiveThumbBmp(minimalHomeCoverWidth(coverHeight),
                                                           minimalHomeCoverHeight(coverHeight), &renderer,
                                                           SETTINGS.getReaderFontId())
                           : epub.generateThumbBmp(0, coverHeight, &renderer, SETTINGS.getReaderFontId()));
            if (!success) {
              if (!epub.hasCoverImage()) markCoverMissing(book);
            } else if (bookIdx < bookUpdated.size()) {
              bookUpdated[bookIdx] = true;  // non-carousel path reuses same tracking
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              showLoadingProgress(10 + progress * progressIncrement);
              const bool success =
                  useDashboardThumb
                      ? xtc.generateThumbBmp(static_cast<uint16_t>(dashboardHomeCoverWidth(coverHeight)),
                                             static_cast<uint16_t>(dashboardHomeCoverHeight(coverHeight)))
                      : (useExactHomeThumb
                             ? xtc.generateThumbBmp(static_cast<uint16_t>(minimalHomeCoverWidth(coverHeight)),
                                                    static_cast<uint16_t>(minimalHomeCoverHeight(coverHeight)))
                             : xtc.generateThumbBmp(coverHeight));
              if (success) {
                if (bookIdx < bookUpdated.size()) bookUpdated[bookIdx] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  // Re-render only the affected slots rather than rebuilding the entire cache.
  if (isCarouselTheme) {
    bool anyUpdated = false;
    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (static_cast<size_t>(i) >= bookUpdated.size() || !bookUpdated[i]) continue;
      anyUpdated = true;
      if (carouselFramesReady) {
        // Only re-render the slot holding this book; books outside the window
        // will be picked up by updateSlidingWindowCache on next navigation.
        const int slot = gCarouselCache.findFrameSlot(i);
        if (slot >= 0) renderCarouselFrame(i, slot);
      }
    }
    if (anyUpdated) {
      if (!carouselFramesReady) {
        // Cover assets changed before the carousel cache was initialised, so
        // any existing SD snapshot may still contain placeholder frames.
        // Force a rebuild from the fresh thumbs instead of reusing stale
        // `home_carousel_cache.bin` content keyed only by book order/layout.
        if (Storage.exists(CAROUSEL_CACHE_PATH)) {
          Storage.remove(CAROUSEL_CACHE_PATH);
        }
        if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
          Storage.remove(CAROUSEL_CACHE_TMP_PATH);
        }
        preRenderCarouselFrames();
      } else {
        // The live carousel frames are already updated above. Keep Home
        // responsive by invalidating any stale SD snapshot instead of
        // rewriting all 5 frames synchronously on this return-to-Home path.
        if (Storage.exists(CAROUSEL_CACHE_PATH)) {
          Storage.remove(CAROUSEL_CACHE_PATH);
        }
        if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
          Storage.remove(CAROUSEL_CACHE_TMP_PATH);
        }
      }
      requestUpdate();
    }
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  hasAo3Library = true;
  hasBookFusion = true;
  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  // Check if any books have bookmarks (directory scan only, no file parsing)
  hasBookmarks = BookmarkStore::hasAnyBookmarks();
  hasClippings = ClippingStore::hasAnyClippings();

  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  carouselCoverTouchDownIndex = -1;
  carouselCoverTouchDownWasSelected = false;
  minimalMenuOpen = false;
  minimalSuppressInitialFrontRelease = usesMinimalHomeInteraction();
  backPressSeen = false;
  minimalMenuIndex = 0;
  minimalHomeNavIndex = -1;
  carouselFramesReady = false;
  carouselWarmupPending = isCarouselTheme;

  // Resolve the calendar day once here rather than per render: the companion's
  // mood has to reflect days elapsed since the last reading session, and this
  // is the only screen that shows it outside one.
  COMPANION.refreshForDisplay();
  // Pick the line once per visit, not per render, so it stays put while the
  // menu cursor moves but is fresh every time you come back. Drawn at random
  // rather than in rotation, and never the same one twice running -- a repeat
  // reads as a bug even when it is chance. Not persisted: an SD write is not
  // worth it for flavour text, and a reshuffle after a reboot is harmless.
  static uint32_t lastQuote = UINT32_MAX;
  const uint8_t quoteCount = companion::quoteCountFor(CompanionTracker::activeId(), COMPANION.currentMood());
  if (quoteCount > 1) {
    uint32_t pick = lastQuote;
    while (pick == lastQuote) pick = esp_random() % quoteCount;
    lastQuote = pick;
    companionQuoteIndex = pick;
  } else {
    companionQuoteIndex = 0;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int recentBooksToLoad =
      std::min(kMaxCachedBooks, std::max(metrics.homeRecentBooksCount, HOME_BOOK_SWAP_RECENT_COUNT));
  RECENT_BOOKS.ensureLoaded();
  loadRecentBooks(recentBooksToLoad);

  const auto selectInitialBook = [this, &metrics](const std::string& path) {
    if (path.empty()) {
      return false;
    }

    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (recentBooks[i].path == path) {
        if (metrics.homeRecentBooksCount == 1 && i > 0) {
          std::rotate(recentBooks.begin(), recentBooks.begin() + i, recentBooks.end());
          selectorIndex = 0;
          lastCarouselBookIndex = 0;
        } else {
          selectorIndex = i;
          lastCarouselBookIndex = i;
        }
        return true;
      }
    }
    return false;
  };

  if (!selectInitialBook(initialBookPath)) {
    selectInitialBook(APP_STATE.openEpubPath);
  }

  globalStats = GlobalReadingStats::load();
  showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
  allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
  if (isCarouselTheme) {
    loadAllBookStats();
  }
  updateHighlightedBookContext(false);

  if (initialMenuItem != HomeMenuItem::NONE) {
    const bool includeContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();
    const auto menuItems = buildSelectableHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats,
                                                        hasBookmarks, hasClippings, includeContinueReading);
    const int menuIndex = findMenuActionIndex(menuItems, homeActionForInitialMenuItem(initialMenuItem));
    if (menuIndex >= 0) {
      selectorIndex = getHomeMenuSelectionOffset(recentBooks) + menuIndex;
    }
  }

  if (isCarouselTheme && hasValidCarouselDiskCache(recentBooks, renderer, hasOpdsServers, hasAo3Library,
                                                   hasBookFusion, hasReadingStats, hasBookmarks, hasClippings)) {
    preRenderCarouselFrames(false);
  }

  requestUpdate();
}

int HomeActivity::getHighlightedBookIndex() const {
  if (recentBooks.empty()) {
    return -1;
  }

  const int visibleBookCount = getVisibleRecentBookCount();
  const int highlightedBookIdx = (selectorIndex < visibleBookCount) ? selectorIndex : lastCarouselBookIndex;
  return std::clamp(highlightedBookIdx, 0, visibleBookCount - 1);
}

int HomeActivity::getVisibleRecentBookCount() const { return ::getVisibleRecentBookCount(recentBooks); }

bool HomeActivity::canSwapHomeBook() const {
  return UITheme::getInstance().getMetrics().homeRecentBooksCount == 1 && recentBooks.size() > 1;
}

void HomeActivity::showNextRecentBookOnHome() {
  if (!canSwapHomeBook()) {
    return;
  }

  std::rotate(recentBooks.begin(), recentBooks.begin() + 1, recentBooks.end());
  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  bookStatsCached = false;
  updateHighlightedBookContext();
  invalidateCoverCache();
  requestUpdate();
}

std::string HomeActivity::getCurrentBookPath() const {
  const int idx = getHighlightedBookIndex();
  return idx >= 0 ? recentBooks[idx].path : std::string{};
}

void HomeActivity::updateHighlightedBookContext(const bool allowEpubLoad) {
  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;
  currentBookChapterTitle.clear();

  const int idx = getHighlightedBookIndex();
  const bool useCachedStats = idx >= 0 && bookStatsCached && idx < kMaxCachedBooks;
  if (idx >= 0) {
    const RecentBook& book = recentBooks[idx];
    const bool isEpub = FsHelpers::hasEpubExtension(book.path);
    const bool loadChapterTitle = isDashboardTheme();
    if (useCachedStats) {
      currentBookStats = cachedBookStats[idx];
      currentBookProgressPercent = cachedBookProgress[idx];
      if (allowEpubLoad && loadChapterTitle && isEpub) {
        loadEpubHighlightedContext(book, false, true, nullptr, &currentBookChapterTitle);
      }
    } else {
      currentBookStats = loadRecentBookStats(book);
      if (isEpub && allowEpubLoad) {
        loadEpubHighlightedContext(book, true, loadChapterTitle, &currentBookProgressPercent, &currentBookChapterTitle);
      } else if (!isEpub) {
        currentBookProgressPercent = RecentBookProgress::loadPercent(book);
      } else {
        currentBookProgressPercent = RecentBookProgress::loadCachedEpubPercent(book);
      }
      if (loadChapterTitle && !isEpub) {
        currentBookChapterTitle.clear();
      }
    }
  }

  hasReadingStats = hasAnyBookStats(currentBookStats) || hasAnyGlobalStats(globalStats) ||
                    (showAllDevicesStats && hasAnyGlobalStats(allDevicesGlobalStats));
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Consume the milestone once the user has actually been on the screen that
  // shows it. Clearing at render time instead would lose it to the very first
  // repaint, before it had been read.
  if (SETTINGS.companionEnabled && COMPANION_STATE.milestonePending) {
    COMPANION_STATE.milestonePending = false;
    COMPANION_STATE.saveToFile();
  }

  // Free the stored cover buffer if any
  freeCoverBuffer();
  gCarouselCache.invalidate();
  freeCarouselFrames();
  carouselWarmupPending = false;
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  if (ESP.getFreeHeap() < needed || ESP.getMaxAllocHeap() < needed) {
    LOG_DBG("HOME", "Skipping cover buffer cache (%zu bytes, free=%u, maxAlloc=%u)", needed, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

void HomeActivity::drawCompanion(const Rect region) const {
  if (!SETTINGS.companionEnabled || !SETTINGS.companionOnHome) return;
  if (region.width <= 0 || region.height <= 0) return;

  const int stripTop = region.y;
  const int stripBottom = region.y + region.height;
  const int pageWidth = region.x + region.width;
  const int leftEdge = region.x;

  constexpr int SIDE_MARGIN = 10;
  constexpr int GAP = 4;          // between the tail tip and the character
  constexpr int BUBBLE_PAD = 10;  // inside the bubble, clear of the rounded corners
  constexpr int TAIL_LENGTH = 12;
  constexpr int MIN_BUBBLE_W = 90;
  constexpr int LABEL_GAP = 2;            // between the character's feet and its status label
  constexpr int SUBLABEL_GAP = 0;         // between the mood label and the streak/progress line
  constexpr int DESCENDER_ALLOWANCE = 3;  // getTextHeight() omits descenders
  constexpr int BOTTOM_MARGIN = 2;        // keeps the last line clear of the button hints
  // Walk cycle. Advanced by redraws rather than a timer: a timer would keep the
  // panel refreshing, block the low-power idle, and accumulate e-ink ghosting.
  // Driving it from input means the character only moves when the screen was
  // going to be repainted anyway, so the motion is free.
  constexpr int WALK_STEPS = 6;
  // Kept to roughly the tail's reach: the bubble has to sit clear of the
  // character's furthest step, so every pixel of pacing pushes the bubble right.
  constexpr int WALK_TRAVEL = 14;
  constexpr int BOB_HEIGHT = 3;
  constexpr int MAX_SCALE = 4;

  const int available = stripBottom - stripTop - BOTTOM_MARGIN;

  // getTextHeight() reports the ascender only, but drawText() takes y as the
  // top and descenders hang below it. Without this allowance the bottom line
  // overruns into the button hints.
  const int labelH = renderer.getTextHeight(UI_10_FONT_ID) + DESCENDER_ALLOWANCE;
  const int subH = renderer.getTextHeight(SMALL_FONT_ID) + DESCENDER_ALLOWANCE;

  // The mood label, and under it the line answering "why?" and "what next?".
  // A reachable target beats a tally, so progress toward Thriving wins when
  // there is progress to report.
  const auto id = CompanionTracker::activeId();
  const auto mood = COMPANION.currentMood();
  const char* label = companion::moodLabel(mood);
  char sub[40] = "";
  const uint16_t minutes = COMPANION.minutesToday();
  const companion::MoodThresholds thresholds;
  if (minutes >= thresholds.contentMinutes && minutes < thresholds.thrivingMinutes) {
    snprintf(sub, sizeof(sub), tr(STR_COMPANION_TO_THRIVING_FORMAT), thresholds.thrivingMinutes - minutes);
  } else if (COMPANION.hasValidClock() && COMPANION_STATE.ledger.streakDays > 0) {
    snprintf(sub, sizeof(sub), tr(STR_COMPANION_STREAK_FORMAT), COMPANION_STATE.ledger.streakDays);
  }

  // A beaten personal best takes over the bubble once, then reverts to the
  // normal mood lines. The flag is cleared by the caller after the render so a
  // repaint mid-visit does not swallow it before it has been seen.
  const char* quote = COMPANION_STATE.milestonePending ? companion::milestoneQuoteFor(id, companionQuoteIndex)
                                                       : companion::quoteFor(id, mood, companionQuoteIndex);

  // Side by side only works if the widest character this could pick still leaves
  // a bubble wide enough to wrap. A column beside the menu does not, so it
  // stacks the bubble above the character instead. Tested against the largest
  // scale rather than the chosen one because the scale is picked on height, and
  // a tall narrow region would otherwise choose a character that squeezes the
  // bubble out entirely.
  constexpr int SIDE_BY_SIDE_MIN_W =
      SIDE_MARGIN * 2 + WALK_TRAVEL + companion::poseWidth(MAX_SCALE) + GAP + TAIL_LENGTH + MIN_BUBBLE_W;
  if (region.width < SIDE_BY_SIDE_MIN_W) {
    drawCompanionColumn(region, label, sub, quote);
    return;
  }

  // Themes leave very different amounts of room under their menus: Lyra spares
  // ~148px, Lyra Extended 90, Classic 78. Stacking the status under the
  // character only works in the first case. Below the height that fits a
  // scale-3 character over both status lines, the stack starves everything at
  // once -- a 30px character beside a bubble too short to hold a single line --
  // so the layout turns on its side instead: the status moves into a column at
  // the right edge and the character and bubble each take the full strip height.
  const int roomyNeeded = companion::poseHeight(3) + BOB_HEIGHT + LABEL_GAP + labelH + SUBLABEL_GAP + subH;
  if (available < roomyNeeded) {
    drawCompanionCompact(stripTop, available, leftEdge, pageWidth, label, sub, quote);
    return;
  }

  // Whole-pixel scales only: fractional scaling would smear the baked dither.
  const int labelBlock = LABEL_GAP + labelH + SUBLABEL_GAP + subH;
  int scale = MAX_SCALE;
  while (scale > 1 && companion::poseHeight(scale) + BOB_HEIGHT + labelBlock > available) scale--;

  const int spriteW = companion::poseWidth(scale);
  const int spriteH = companion::poseHeight(scale);

  // Character and labels are centred as one block, so the status sits directly
  // under the feet instead of drifting to the bottom of the strip.
  const int blockH = spriteH + BOB_HEIGHT + labelBlock;
  const int blockTop = stripTop + std::max(0, (available - blockH) / 2);
  const int artHeight = spriteH + BOB_HEIGHT;

  // Ping-pong across WALK_TRAVEL, facing the direction of travel.
  const uint32_t phase = companionFrame % (WALK_STEPS * 2);
  const bool walkingBack = phase >= WALK_STEPS;
  const uint32_t step = walkingBack ? (WALK_STEPS * 2 - 1 - phase) : phase;
  const int walkX = static_cast<int>(step) * WALK_TRAVEL / (WALK_STEPS - 1);
  const int bob = (companionFrame % 2) ? BOB_HEIGHT : 0;

  const int spriteX = leftEdge + SIDE_MARGIN + walkX;
  const int spriteY = blockTop + bob;

  // A neglected companion is curled up or powered down: pacing about would
  // undercut the pose, so it stays put and only the quote rotates.
  const bool restless = mood != companion::Mood::Neglected;
  const int drawY = restless ? spriteY : spriteY - bob;
  companion::drawPose(renderer, id, mood, restless ? spriteX : leftEdge + SIDE_MARGIN, drawY, scale,
                      restless && walkingBack);

  // Status lines sit centred under the character's whole pacing range, not
  // under the sprite itself, so they stay put while the character moves.
  const int lane = WALK_TRAVEL + spriteW;
  {
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    const int subW = sub[0] != '\0' ? renderer.getTextWidth(SMALL_FONT_ID, sub) : 0;

    // Both lines share one centre so they read as a block. That centre starts
    // under the character but is pushed right far enough that the widest line
    // still clears the screen edge -- "27 min to Thriving" is wider than the
    // character it sits under, so centring on the sprite alone runs it off.
    const int widest = std::max(labelW, subW);
    int centreX = leftEdge + SIDE_MARGIN + lane / 2;
    centreX = std::max(centreX, leftEdge + SIDE_MARGIN + widest / 2);
    centreX = std::min(centreX, pageWidth - SIDE_MARGIN - widest / 2);

    const int labelY = blockTop + artHeight + LABEL_GAP;
    renderer.drawText(UI_10_FONT_ID, centreX - labelW / 2, labelY, label, true, EpdFontFamily::BOLD);
    if (subW > 0) {
      renderer.drawText(SMALL_FONT_ID, centreX - subW / 2, labelY + labelH + SUBLABEL_GAP, sub);
    }
  }

  if (!quote) return;

  // Bubble body starts past the character's furthest step plus the tail, so the
  // two can never collide mid-stride.
  const int bubbleX = leftEdge + SIDE_MARGIN + WALK_TRAVEL + spriteW + GAP + TAIL_LENGTH;
  const int bubbleW = pageWidth - bubbleX - SIDE_MARGIN;
  if (bubbleW < MIN_BUBBLE_W) return;  // narrow screen: character only

  // Small font, not UI_10: the quote pool runs up to ~50 characters and the
  // narrower glyphs are what keep a long line to three rows instead of
  // needing a fourth that drawCenteredWrappedText would silently ellipsis
  // away. Measured here (rather than left to drawCenteredWrappedText) so the
  // bubble can be sized to the lines the quote actually needs.
  const int quoteLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int textW = bubbleW - BUBBLE_PAD * 2;
  std::vector<std::string> lines;
  if (renderer.getTextWidth(SMALL_FONT_ID, quote) <= textW) {
    lines.emplace_back(quote);
  } else {
    lines = renderer.wrappedText(SMALL_FONT_ID, quote, textW, 3);
  }
  if (lines.empty()) return;

  // Anchored to the character's band, not to the bobbing sprite, so the text
  // stays still while the character moves under it. Sized to the measured
  // lines rather than a flat spriteH guess, so it never clips what it drew.
  const int bubbleH = std::min(artHeight, static_cast<int>(lines.size()) * quoteLineH + BUBBLE_PAD * 2);
  const int bubbleY = blockTop + (artHeight - bubbleH) / 2;

  companion::drawSpeechBubble(renderer, bubbleX, bubbleY, bubbleW, bubbleH, TAIL_LENGTH);

  const Rect textBounds{bubbleX + BUBBLE_PAD, bubbleY, textW, bubbleH};
  int textY = bubbleY + BUBBLE_PAD;
  for (const auto& line : lines) {
    UITheme::drawCenteredText(renderer, textBounds, SMALL_FONT_ID, textY, line.c_str());
    textY += quoteLineH;
  }
}

void HomeActivity::drawCompanionCompact(const int stripTop, const int available, const int leftEdge,
                                        const int pageWidth, const char* label, const char* sub,
                                        const char* quote) const {
  constexpr int SIDE_MARGIN = 10;
  constexpr int GAP = 4;
  constexpr int TAIL_LENGTH = 12;
  constexpr int PAD = 6;             // bubble inner padding, tighter than the roomy layout's 10
  constexpr int COL_GAP = 8;         // between the bubble and the status column
  constexpr int MIN_BUBBLE_W = 200;  // below this the quote wraps to nonsense, so the column goes instead
  constexpr int STRIP_INSET = 4;     // keeps the bubble off the menu row above and the hints below
  constexpr int SUBLABEL_GAP = 0;
  constexpr int DESCENDER_ALLOWANCE = 3;
  constexpr int WALK_STEPS = 6;
  constexpr int WALK_TRAVEL = 14;
  constexpr int BOB_HEIGHT = 3;
  constexpr int MAX_SCALE = 4;

  // Nothing is stacked here, so the character is free to take the whole strip.
  int scale = 0;
  for (int candidate = MAX_SCALE; candidate >= 1; candidate--) {
    if (companion::poseHeight(candidate) + BOB_HEIGHT <= available) {
      scale = candidate;
      break;
    }
  }
  if (scale == 0) return;  // theme leaves no usable room

  const int spriteW = companion::poseWidth(scale);
  const int spriteH = companion::poseHeight(scale);
  const int spriteTop = stripTop + (available - spriteH - BOB_HEIGHT) / 2;

  const uint32_t phase = companionFrame % (WALK_STEPS * 2);
  const bool walkingBack = phase >= WALK_STEPS;
  const uint32_t step = walkingBack ? (WALK_STEPS * 2 - 1 - phase) : phase;
  const int walkX = static_cast<int>(step) * WALK_TRAVEL / (WALK_STEPS - 1);
  const int bob = (companionFrame % 2) ? BOB_HEIGHT : 0;

  const auto id = CompanionTracker::activeId();
  const auto mood = COMPANION.currentMood();
  const bool restless = mood != companion::Mood::Neglected;
  companion::drawPose(renderer, id, mood, leftEdge + SIDE_MARGIN + (restless ? walkX : 0),
                      restless ? spriteTop + bob : spriteTop, scale, restless && walkingBack);

  if (!quote) return;

  // The bubble never spans the whole strip: sitting flush under the last menu
  // row reads as a collision even when the arithmetic is right.
  const int usableH = available - STRIP_INSET * 2;
  const int bigLineH = renderer.getLineHeight(UI_10_FONT_ID);

  // On the shortest strips a UI_10 bubble holds one line, which truncates the
  // longer quotes, and a full-size mood label towers over a scale-1 character.
  // Both drop to the small font together, so the text stays subordinate to the
  // companion and every quote survives intact.
  const bool tight = usableH < bigLineH * 2 + PAD * 2;
  const int quoteFont = tight ? SMALL_FONT_ID : UI_10_FONT_ID;
  const int labelFont = tight ? SMALL_FONT_ID : UI_10_FONT_ID;

  const int labelH = renderer.getTextHeight(labelFont) + DESCENDER_ALLOWANCE;
  const int subH = renderer.getTextHeight(SMALL_FONT_ID) + DESCENDER_ALLOWANCE;
  const int labelW = renderer.getTextWidth(labelFont, label, EpdFontFamily::BOLD);
  const int subW = sub[0] != '\0' ? renderer.getTextWidth(SMALL_FONT_ID, sub) : 0;

  const int bubbleX = leftEdge + SIDE_MARGIN + WALK_TRAVEL + spriteW + GAP + TAIL_LENGTH;
  int bubbleRight = pageWidth - SIDE_MARGIN;

  // Status column at the right edge, dropped whole rather than clipped: it only
  // earns its width if the bubble still has room to wrap sensibly.
  const int statusRows = (available >= labelH + SUBLABEL_GAP + subH && subW > 0) ? 2 : (available >= labelH ? 1 : 0);
  const int statusW = statusRows >= 2 ? std::max(labelW, subW) : labelW;
  const bool showStatus = statusRows > 0 && bubbleRight - statusW - COL_GAP - bubbleX >= MIN_BUBBLE_W;
  if (showStatus) bubbleRight -= statusW + COL_GAP;

  const int bubbleW = bubbleRight - bubbleX;
  if (bubbleW < MIN_BUBBLE_W) return;  // narrow screen: character only

  // Wrapped here rather than inside drawCenteredWrappedText so the bubble can be
  // sized to the lines the quote actually needs. Stretching it to the full strip
  // instead just crowds the menu row above.
  const int lineH = renderer.getLineHeight(quoteFont);
  const int textW = bubbleW - PAD * 2;
  const int maxLines = std::max(1, std::min(3, (usableH - PAD * 2) / lineH));
  std::vector<std::string> lines;
  if (renderer.getTextWidth(quoteFont, quote) <= textW) {
    lines.emplace_back(quote);
  } else {
    lines = renderer.wrappedText(quoteFont, quote, textW, maxLines);
  }
  if (lines.empty()) return;

  const int bubbleH = static_cast<int>(lines.size()) * lineH + PAD * 2;
  const int bubbleY = stripTop + (available - bubbleH) / 2;
  companion::drawSpeechBubble(renderer, bubbleX, bubbleY, bubbleW, bubbleH, TAIL_LENGTH);

  const Rect textBounds{bubbleX + PAD, bubbleY, textW, bubbleH};
  int textY = bubbleY + PAD;
  for (const auto& line : lines) {
    UITheme::drawCenteredText(renderer, textBounds, quoteFont, textY, line.c_str());
    textY += lineH;
  }

  if (!showStatus) return;

  const int statusH = statusRows >= 2 ? labelH + SUBLABEL_GAP + subH : labelH;
  const int statusX = pageWidth - SIDE_MARGIN - statusW;
  const int statusTop = stripTop + (available - statusH) / 2;
  renderer.drawText(labelFont, statusX + (statusW - labelW) / 2, statusTop, label, true, EpdFontFamily::BOLD);
  if (statusRows >= 2) {
    renderer.drawText(SMALL_FONT_ID, statusX + (statusW - subW) / 2, statusTop + labelH + SUBLABEL_GAP, sub);
  }
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::invalidateCoverCache() {
  coverRendered = false;
  freeCoverBuffer();
}

void HomeActivity::freeCarouselFrames() {
  // Instance pointers are aliases into the static cache — do not free here.
  for (int i = 0; i < kCarouselFrameCount; ++i) carouselFrames[i] = nullptr;
  carouselFramesReady = false;
}

bool HomeActivity::allocateCarouselFrameSlots(int targetFrameCount) {
  const size_t bufferSize = renderer.getBufferSize();
  int frameCount = 0;
  for (int attemptFrameCount = targetFrameCount; attemptFrameCount >= 1; --attemptFrameCount) {
    bool allocFailed = false;
    for (int i = 0; i < attemptFrameCount; ++i) {
      gCarouselCache.frames[i] = static_cast<uint8_t*>(malloc(bufferSize));
      if (!gCarouselCache.frames[i]) {
        LOG_ERR("HOME", "preRenderCarouselFrames: malloc failed for frame %d while allocating %d frame(s)", i,
                attemptFrameCount);
        allocFailed = true;
        break;
      }
      if (!hasHeapForCarouselFrameCache()) {
        LOG_INF("HOME", "carousel: low heap after frame cache alloc (%u free, %u maxAlloc); skipping cache",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        free(gCarouselCache.frames[i]);
        gCarouselCache.frames[i] = nullptr;
        allocFailed = true;
        break;
      }
      gCarouselCache.frameBookIdx[i] = -1;
    }

    if (!allocFailed) {
      frameCount = attemptFrameCount;
      break;
    }

    for (int i = 0; i < attemptFrameCount; ++i) {
      if (gCarouselCache.frames[i]) {
        free(gCarouselCache.frames[i]);
        gCarouselCache.frames[i] = nullptr;
      }
      gCarouselCache.frameBookIdx[i] = -1;
    }
  }

  if (frameCount == 0) {
    gCarouselCache.invalidate();
    return false;
  }

  gCarouselCache.frameCount = frameCount;
  LOG_INF("HOME", "carousel: frame cache capacity %d/%d", frameCount, targetFrameCount);
  return true;
}

void HomeActivity::renderCarouselFrameToCurrentBuffer(int bookIdx, BookReadingStats* outStats,
                                                      float* outProgressPercent, bool* outUsedCachedStats) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int bookCount = static_cast<int>(recentBooks.size());
  bool dummy1 = false, dummy2 = false, dummy3 = false;
  BookReadingStats frameStats;
  const BookReadingStats* frameStatsPtr = nullptr;
  float frameProgressPercent = -1.0f;
  bool usedCachedStats = false;

  if (bookIdx >= 0 && bookIdx < bookCount) {
    if (bookStatsCached && bookIdx < kMaxCachedBooks) {
      usedCachedStats = true;
      frameStats = cachedBookStats[bookIdx];
      frameProgressPercent = cachedBookProgress[bookIdx];
    } else {
      frameStats = loadRecentBookStats(recentBooks[bookIdx]);
      frameProgressPercent = RecentBookProgress::loadPercent(recentBooks[bookIdx]);
    }
    if (hasAnyBookStats(frameStats)) frameStatsPtr = &frameStats;
  }

  LyraCarouselTheme::setPreRenderIndex(bookIdx);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
  GUI.drawRecentBookCover(
      renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks, bookCount, dummy1,
      dummy2, dummy3, []() { return true; }, frameStatsPtr, frameProgressPercent);

  const bool frameHasReadingStats = hasAnyBookStats(frameStats) || hasAnyGlobalStats(globalStats) ||
                                    (showAllDevicesStats && hasAnyGlobalStats(allDevicesGlobalStats));
  const auto menuItems =
      buildHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, frameHasReadingStats, hasBookmarks,
                        hasClippings);
  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), -1, [&menuItems](int index) { return menuItems[index].label; },
      [&menuItems](int index) { return menuItems[index].icon; });

  const auto labels = mappedInput.mapLabels(tr(STR_READ), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (outStats) *outStats = frameStats;
  if (outProgressPercent) *outProgressPercent = frameProgressPercent;
  if (outUsedCachedStats) *outUsedCachedStats = usedCachedStats;
}

bool HomeActivity::buildCarouselCacheFile(const std::string& cacheKey, uint64_t cacheKeyHash, int bookCount,
                                          bool showProgressPopup) {
  (void)cacheKey;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || bookCount <= 0) return false;

  Storage.mkdir("/.crosspoint");
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }

  FsFile file;
  if (!Storage.openFileForWrite("HOME", CAROUSEL_CACHE_TMP_PATH, file)) {
    return false;
  }

  const CarouselCacheHeader header = {
      CAROUSEL_CACHE_MAGIC,
      CAROUSEL_CACHE_VERSION,
      static_cast<uint16_t>(bookCount),
      static_cast<uint32_t>(renderer.getBufferSize()),
      cacheKeyHash,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbW),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbH),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverW),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverH),
  };
  if (!serialization::tryWritePod(file, header)) {
    file.close();
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache header");
    return false;
  }

  const auto start = millis();
  Rect popupRect{};
  uint8_t* progressFrameBuffer = nullptr;
  const size_t bufferSize = renderer.getBufferSize();
  if (showProgressPopup) {
    progressFrameBuffer = static_cast<uint8_t*>(malloc(bufferSize));
    if (!progressFrameBuffer) {
      LOG_ERR("HOME", "carousel: failed to allocate progress overlay buffer");
      showProgressPopup = false;
    } else {
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
      memcpy(progressFrameBuffer, frameBuffer, bufferSize);
    }
  }
  bool writeFailed = false;
  for (int i = 0; i < bookCount; ++i) {
    const int cachedSlot = gCarouselCache.findFrameSlot(i);
    if (cachedSlot >= 0 && carouselFrames[cachedSlot]) {
      memcpy(frameBuffer, carouselFrames[cachedSlot], renderer.getBufferSize());
    } else {
      renderCarouselFrameToCurrentBuffer(i, nullptr, nullptr, nullptr);
    }
    if (file.write(frameBuffer, renderer.getBufferSize()) != renderer.getBufferSize()) {
      writeFailed = true;
      break;
    }
    if (showProgressPopup) {
      memcpy(frameBuffer, progressFrameBuffer, bufferSize);
      GUI.fillPopupProgress(renderer, popupRect, ((i + 1) * 100) / bookCount);
    }
  }

  const bool syncOk = file.sync();
  file.close();

  if (writeFailed || !syncOk) {
    free(progressFrameBuffer);
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache snapshot");
    return false;
  }

  if (Storage.exists(CAROUSEL_CACHE_PATH)) {
    Storage.remove(CAROUSEL_CACHE_PATH);
  }
  if (!Storage.rename(CAROUSEL_CACHE_TMP_PATH, CAROUSEL_CACHE_PATH)) {
    free(progressFrameBuffer);
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to promote SD cache snapshot");
    return false;
  }

  free(progressFrameBuffer);
  LOG_DBG("HOME", "carousel: built SD cache for %d book(s) in %lums", bookCount, millis() - start);
  return true;
}

bool HomeActivity::loadCarouselFrameFromDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= kCarouselFrameCount || !gCarouselCache.frames[slotIdx] || bookIdx < 0 ||
      bookIdx >= bookCount) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, file)) {
    return false;
  }

  CarouselCacheHeader header{};
  if (!readCarouselCacheHeader(file, header) ||
      !isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer)) {
    file.close();
    return false;
  }

  const size_t frameOffset = sizeof(CarouselCacheHeader) + static_cast<size_t>(bookIdx) * renderer.getBufferSize();
  if (!file.seek(frameOffset)) {
    file.close();
    return false;
  }
  const size_t expectedBytes = renderer.getBufferSize();
  size_t totalBytesRead = 0;
  while (totalBytesRead < expectedBytes) {
    const int bytesRead = file.read(gCarouselCache.frames[slotIdx] + totalBytesRead, expectedBytes - totalBytesRead);
    if (bytesRead <= 0) {
      break;
    }
    totalBytesRead += static_cast<size_t>(bytesRead);
  }
  file.close();
  if (totalBytesRead != expectedBytes) {
    LOG_ERR("HOME", "carousel: short read for slot %d (%zu/%zu bytes)", slotIdx, totalBytesRead, expectedBytes);
    return false;
  }

  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
  return true;
}

int HomeActivity::chooseCarouselEvictionSlot(int centerIdx, int bookCount, std::optional<int> protectedBookIdx) const {
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (gCarouselCache.frames[i] && gCarouselCache.frameBookIdx[i] < 0) {
      return i;
    }
  }

  int evictSlot = -1;
  int maxDist = -1;
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (!gCarouselCache.frames[i]) continue;
    const int cachedBookIdx = gCarouselCache.frameBookIdx[i];
    if (protectedBookIdx.has_value() && cachedBookIdx == protectedBookIdx.value()) continue;
    const int diff = std::abs(cachedBookIdx - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }
  return evictSlot;
}

bool HomeActivity::preRenderCarouselFrames(bool showProgressPopup) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) return false;
  bool showedProgressPopup = false;

  // Build cache key from book paths plus thumb-asset availability so we don't
  // reuse a stale snapshot built before carousel-sized thumbs existed.
  std::string newKey;
  uint64_t newKeyHash = 0;
  buildCarouselCacheKey(recentBooks, hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings, newKey,
                        newKeyHash);

  // Cache hit: same books in same order — reuse without any SD reads
  if (newKey == gCarouselCache.key && gCarouselCache.frameCount > 0) {
    for (int i = 0; i < gCarouselCache.frameCount; ++i) carouselFrames[i] = gCarouselCache.frames[i];
    carouselFramesReady = true;
    coverRendered = false;
    coverBufferStored = false;
    return false;
  }

  // Cache miss: free old cache and re-render
  if (!renderer.getFrameBuffer()) return false;
  freeCoverBuffer();  // reclaim 48KB before allocating frames
  gCarouselCache.invalidate();

  const int targetFrameCount = std::min(bookCount, kCarouselFrameCount);
  bool diskCacheValid = false;
  FsFile cacheFile;
  if (Storage.openFileForRead("HOME", CAROUSEL_CACHE_PATH, cacheFile)) {
    CarouselCacheHeader header{};
    const bool readOk = readCarouselCacheHeader(cacheFile, header);
    cacheFile.close();
    diskCacheValid = readOk && isCarouselCacheHeaderValid(header, newKeyHash, bookCount, renderer);
  }

  if (!allocateCarouselFrameSlots(targetFrameCount)) {
    return showedProgressPopup;
  }

  // Keep only the current frame in RAM; adjacent frames come from the SD
  // snapshot on demand instead of occupying another framebuffer-sized slot.
  const int selectedBookIdx = (selectorIndex < bookCount) ? selectorIndex : lastCarouselBookIndex;
  const int initialBookIdx = (selectedBookIdx >= 0 && selectedBookIdx < bookCount) ? selectedBookIdx : 0;
  auto loadOrRender = [&](int bookIdx, int slot) {
    if (!diskCacheValid || !loadCarouselFrameFromDisk(newKeyHash, bookCount, bookIdx, slot)) {
      renderCarouselFrame(bookIdx, slot);
    }
  };
  loadOrRender(initialBookIdx, 0);
  gCarouselCache.lastCenterIdx = initialBookIdx;

  if (gCarouselCache.frameCount >= 2 && bookCount >= 2) {
    const int nextIdx = (initialBookIdx + 1) % bookCount;
    loadOrRender(nextIdx, 1);
  }

  if (gCarouselCache.frameCount >= 3 && bookCount >= 3) {
    const int prevIdx = (initialBookIdx + bookCount - 1) % bookCount;
    loadOrRender(prevIdx, 2);
  }

  const bool hasFullFrameCache = gCarouselCache.frameCount >= targetFrameCount;
  gCarouselCache.key = newKey;
  gCarouselCache.keyHash = diskCacheValid ? newKeyHash : 0;
  carouselFramesReady = true;
  coverRendered = false;
  coverBufferStored = false;

  // Persist the freshly-rendered carousel snapshot back to SD after Home is
  // already visible so later reader->Home returns and carousel navigation can
  // bootstrap from disk instead of live-rendering covers again.
  if (!diskCacheValid && gCarouselCache.frameCount > 0) {
    if (hasFullFrameCache) {
      const bool cacheBuilt = buildCarouselCacheFile(newKey, newKeyHash, bookCount, showProgressPopup);
      if (cacheBuilt) {
        gCarouselCache.keyHash = newKeyHash;
        showedProgressPopup = true;
      }
    } else {
      LOG_INF("HOME", "carousel: skipping SD cache build in degraded frame cache mode");
    }
  }
  return showedProgressPopup;
}

void HomeActivity::loop() {
  if (quickActionsLongPowerHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Power)) {
      quickActionsLongPowerHandled = false;
    }
    return;
  }

  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS &&
      mappedInput.isPressed(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    quickActionsLongPowerHandled = true;
    handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS);
    return;
  }

  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (usesMinimalHomeInteraction()) {
    const int pressedFrontButton = mappedInput.getPressedFrontButton();
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    if (minimalSuppressInitialFrontRelease) {
      if (releasedFrontButton >= 0) {
        minimalSuppressInitialFrontRelease = false;
        return;
      }
      if (isAnyFrontButtonPressed(mappedInput)) {
        return;
      }
      minimalSuppressInitialFrontRelease = false;
    }

    if (homeBookSwapLongPressHandled) {
      if (releasedFrontButton == HalGPIO::BTN_BACK || !mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK)) {
        homeBookSwapLongPressHandled = false;
      }
      return;
    }

    if (minimalMenuOpen) {
      const auto menuItems =
          buildMinimalMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings);
      const int menuCount = static_cast<int>(menuItems.size());
      if (menuCount <= 0) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }

      if (minimalMenuIndex >= menuCount) {
        minimalMenuIndex = menuCount - 1;
      }

      auto activateMinimalMenuAction = [this, &menuItems]() {
        switch (menuItems[minimalMenuIndex].action) {
          case HomeMenuAction::BrowseFiles:
            onFileBrowserOpen();
            break;
          case HomeMenuAction::RecentBooks:
            onRecentsOpen();
            break;
          case HomeMenuAction::OpdsBrowser:
            onOpdsBrowserOpen();
            break;
          case HomeMenuAction::Ao3Library:
            onAo3LibraryOpen();
            break;
          case HomeMenuAction::BookFusion:
            onBookFusionOpen();
            break;
          case HomeMenuAction::ReadingStats:
            onReadingStatsOpen();
            break;
          case HomeMenuAction::Bookmarks:
            onSavedItemsOpen();
            break;
          case HomeMenuAction::FileTransfer:
            onFileTransferOpen();
            break;
          case HomeMenuAction::ContinueReading:
          case HomeMenuAction::Settings:
            break;
        }
      };

      int touchedMenuIndex = -1;
      if (mappedInput.wasItemTouchedDown(touchedMenuIndex) && touchedMenuIndex >= 0 && touchedMenuIndex < menuCount) {
        if (minimalMenuIndex != touchedMenuIndex) {
          minimalMenuIndex = touchedMenuIndex;
          requestUpdate();
        }
        return;
      }
      if (mappedInput.wasItemTapped(touchedMenuIndex) && touchedMenuIndex >= 0 && touchedMenuIndex < menuCount) {
        minimalMenuIndex = touchedMenuIndex;
        activateMinimalMenuAction();
        return;
      }

      int touchX = 0;
      int touchY = 0;
      if (mappedInput.wasScreenTouchDown(touchX, touchY) &&
          !containsPoint(MinimalTheme::buttonMenuPanelRect(renderer, menuCount), touchX, touchY)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }

      buttonNavigator.onPreviousPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onNextPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activateMinimalMenuAction();
      }
      return;
    }

    switch (mappedInput.wasSwipe()) {
      case MappedInputManager::SwipeDir::Down:
        minimalHomeNavIndex = 2;
        onSettingsOpen();
        return;
      case MappedInputManager::SwipeDir::Right:
        minimalHomeNavIndex = 1;
        onFileBrowserOpen();
        return;
      case MappedInputManager::SwipeDir::Up:
        minimalHomeNavIndex = 0;
        minimalMenuOpen = true;
        minimalMenuIndex = 0;
        requestUpdate();
        return;
      case MappedInputManager::SwipeDir::Left:
        if (mappedInput.hasTouch() && canSwapHomeBook()) {
          showNextRecentBookOnHome();
          return;
        }
        break;
      case MappedInputManager::SwipeDir::None:
        break;
    }

    if (canSwapHomeBook() && mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) &&
        mappedInput.getHeldTime() >= HOME_BOOK_SWAP_LONG_PRESS_MS) {
      homeBookSwapLongPressHandled = true;
      showNextRecentBookOnHome();
      return;
    }

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0 ? homeNavCount - 1
                                                    : ButtonNavigator::previousIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0 ? 0 : ButtonNavigator::nextIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }

    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1:
          onFileBrowserOpen();
          break;
        case 2:
          onSettingsOpen();
          break;
        case 3:
          onContinueReading();
          break;
      }
    };

    int touchedHomeNav = -1;
    if (mappedInput.wasItemTouchedDown(touchedHomeNav) && touchedHomeNav >= 0 && touchedHomeNav < homeNavCount) {
      if (minimalHomeNavIndex != touchedHomeNav) {
        minimalHomeNavIndex = touchedHomeNav;
        requestUpdate();
      }
      return;
    }
    int touchedBookIndex = -1;
    if (mappedInput.wasCoverTouchedDown(touchedBookIndex) && touchedBookIndex >= 0 && !recentBooks.empty()) {
      if (minimalHomeNavIndex != 3) {
        minimalHomeNavIndex = 3;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasItemTapped(touchedHomeNav) && touchedHomeNav >= 0 && touchedHomeNav < homeNavCount) {
      minimalHomeNavIndex = touchedHomeNav;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (mappedInput.wasCoverTapped(touchedBookIndex) && touchedBookIndex >= 0 && !recentBooks.empty()) {
      minimalHomeNavIndex = 3;
      onContinueReading();
      return;
    }

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      minimalHomeNavIndex = 0;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_CONFIRM) {
      minimalHomeNavIndex = 1;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_LEFT) {
      minimalHomeNavIndex = 2;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        minimalHomeNavIndex = 3;
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onContinueReading();
      return;
    }
    return;
  }

  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const bool carouselTouchOnly = isCarousel && mappedInput.hasTouchHardware();
  const int previousHighlightedBookIdx = getHighlightedBookIndex();
  const int visibleBookCount = getVisibleRecentBookCount();
  const int carouselMenuItemCount =
      isCarousel
          ? static_cast<int>(
                buildHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings).size())
          : 0;

  MappedInputManager::SwipeDir carouselSwipe = MappedInputManager::SwipeDir::None;
  int carouselSwipeStartX = 0;
  int carouselSwipeStartY = 0;
  int carouselSwipeEndX = 0;
  int carouselSwipeEndY = 0;
  const bool hasCarouselSwipe =
      carouselTouchOnly && mappedInput.wasSwipeWithPoints(carouselSwipe, carouselSwipeStartX, carouselSwipeStartY,
                                                          carouselSwipeEndX, carouselSwipeEndY);
  const bool carouselSwipeStartsInMenu =
      hasCarouselSwipe && containsPoint(LyraCarouselTheme::buttonMenuTouchRect(renderer, carouselMenuItemCount),
                                        carouselSwipeStartX, carouselSwipeStartY);

  // A touch swipe can also satisfy the generic Back gesture. Keep it in the
  // carousel path so a left-edge swipe cannot open the selected book instead.
  if (!hasCarouselSwipe && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    backPressSeen = true;
  }

  // Minimal and Dashboard already returned through their dedicated home
  // interaction path above. On other themes, Back opens the most recent book.
  // Requiring a press observed on Home ignores the stale release that can
  // arrive after Back closed the previous activity.
  if (!carouselTouchOnly && !hasCarouselSwipe && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      backPressSeen && !recentBooks.empty()) {
    onContinueReading();
    return;
  }

  auto activateHomeMenuAction = [this](const HomeMenuAction action) {
    switch (action) {
      case HomeMenuAction::BrowseFiles:
        onFileBrowserOpen();
        break;
      case HomeMenuAction::ContinueReading:
        onContinueReading();
        break;
      case HomeMenuAction::RecentBooks:
        onRecentsOpen();
        break;
      case HomeMenuAction::OpdsBrowser:
        onOpdsBrowserOpen();
        break;
      case HomeMenuAction::Ao3Library:
        onAo3LibraryOpen();
        break;
      case HomeMenuAction::BookFusion:
        onBookFusionOpen();
        break;
      case HomeMenuAction::ReadingStats:
        onReadingStatsOpen();
        break;
      case HomeMenuAction::Bookmarks:
        onSavedItemsOpen();
        break;
      case HomeMenuAction::FileTransfer:
        onFileTransferOpen();
        break;
      case HomeMenuAction::Settings:
        onSettingsOpen();
        break;
    }
  };

  auto activateSelectedHomeItem = [this, visibleBookCount, &activateHomeMenuAction]() {
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!metrics.homeContinueReadingInMenu && selectorIndex < visibleBookCount) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }

    auto menuItems =
        buildSelectableHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings,
                                     metrics.homeContinueReadingInMenu && !recentBooks.empty());
    const int menuSelectedIndex = selectorIndex - getHomeMenuSelectionOffset(recentBooks);
    if (menuSelectedIndex < 0 || menuSelectedIndex >= static_cast<int>(menuItems.size())) {
      return;
    }

    activateHomeMenuAction(menuItems[menuSelectedIndex].action);
  };

  if (homeBookSwapLongPressHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      homeBookSwapLongPressHandled = false;
    }
    return;
  }

  if (!isCarousel && canSwapHomeBook() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= HOME_BOOK_SWAP_LONG_PRESS_MS) {
    homeBookSwapLongPressHandled = true;
    showNextRecentBookOnHome();
    return;
  }

  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA &&
      mappedInput.hasTouch() && canSwapHomeBook() && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) {
    showNextRecentBookOnHome();
    return;
  }

  if (isCarousel) {
    const int bookCount = visibleBookCount;
    const int menuItemCount = carouselMenuItemCount;
    bool inCarouselRow = (selectorIndex < bookCount);
    const int menuIdx = inCarouselRow ? 0 : (selectorIndex - bookCount);

    auto handleTouch = [&](const bool activate) {
      int touchedMenuIndex = -1;
      // Carousel menu icons have no pressed/highlight state. A completed tap
      // is the only menu interaction that changes selection or opens an item.
      if (activate && mappedInput.wasItemTapped(touchedMenuIndex)) {
        if (touchedMenuIndex < 0 || touchedMenuIndex >= menuItemCount) return false;
        const int previousSelectorIndex = selectorIndex;
        selectorIndex = bookCount + touchedMenuIndex;
        if (selectorIndex != previousSelectorIndex) {
          invalidateCoverCache();
        }
        activateSelectedHomeItem();
        return true;
      }

      int bookIndex = -1;
      if (bookCount > 0 &&
          (activate ? mappedInput.wasCoverTapped(bookIndex) : mappedInput.wasCoverTouchedDown(bookIndex))) {
        bookIndex = std::clamp(bookIndex, 0, bookCount - 1);
        const int previousSelectorIndex = selectorIndex;
        const bool wasSelectedAtTouchStart = !activate && inCarouselRow && previousSelectorIndex == bookIndex;
        const bool shouldActivateBook =
            activate && ((carouselCoverTouchDownIndex == bookIndex && carouselCoverTouchDownWasSelected) ||
                         (carouselCoverTouchDownIndex < 0 && inCarouselRow && previousSelectorIndex == bookIndex));
        selectorIndex = bookIndex;
        lastCarouselBookIndex = bookIndex;
        if (!activate) {
          carouselCoverTouchDownIndex = bookIndex;
          carouselCoverTouchDownWasSelected = wasSelectedAtTouchStart;
        } else {
          carouselCoverTouchDownIndex = -1;
          carouselCoverTouchDownWasSelected = false;
        }
        if (selectorIndex != previousSelectorIndex) {
          invalidateCoverCache();
          // Touch-down returns early so the selected cover can repaint before
          // the finger lifts; keep the stats/context used by the next action
          // in sync with that new selection.
          updateHighlightedBookContext(false);
        }
        if (shouldActivateBook) {
          activateSelectedHomeItem();
        } else if (selectorIndex != previousSelectorIndex) {
          requestUpdate();
        }
        return true;
      }
      return false;
    };

    if (!hasCarouselSwipe && handleTouch(/*activate=*/false)) {
      return;
    }
    if (!hasCarouselSwipe && handleTouch(/*activate=*/true)) {
      return;
    }

    auto moveRight = [&]() {
      if (inCarouselRow && bookCount > 0) {
        selectorIndex = (selectorIndex + 1) % bookCount;
        lastCarouselBookIndex = selectorIndex;
      } else if (!inCarouselRow) {
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      }
      requestUpdate();
    };
    auto moveLeft = [&]() {
      if (inCarouselRow && bookCount > 0) {
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
        lastCarouselBookIndex = selectorIndex;
      } else if (!inCarouselRow) {
        selectorIndex = bookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      }
      requestUpdate();
    };

    bool handledHorizontalNav = false;
    if (hasCarouselSwipe) {
      // A swipe that starts in the icon strip is consumed. It must not select
      // an icon or turn into carousel navigation as it passes through one.
      if (carouselSwipeStartsInMenu) return;

      switch (carouselSwipe) {
        case MappedInputManager::SwipeDir::Left:
        case MappedInputManager::SwipeDir::Right:
          if (bookCount <= 0) return;
          if (!inCarouselRow) {
            selectorIndex = std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
            lastCarouselBookIndex = selectorIndex;
            inCarouselRow = true;
            invalidateCoverCache();
          }
          if (carouselSwipe == MappedInputManager::SwipeDir::Left) {
            moveRight();
          } else {
            moveLeft();
          }
          handledHorizontalNav = true;
          break;
        case MappedInputManager::SwipeDir::None:
        case MappedInputManager::SwipeDir::Up:
        case MappedInputManager::SwipeDir::Down:
          break;
      }
    }

    if (!carouselTouchOnly) {
      if (!handledHorizontalNav && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        moveRight();
      }
      if (!handledHorizontalNav && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        moveLeft();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        if (inCarouselRow) {
          lastCarouselBookIndex = selectorIndex;
          selectorIndex = bookCount;
          invalidateCoverCache();
        } else {
          selectorIndex = lastCarouselBookIndex;
          invalidateCoverCache();
        }
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        if (inCarouselRow) {
          lastCarouselBookIndex = selectorIndex;
          selectorIndex = bookCount;
          invalidateCoverCache();
        } else {
          selectorIndex = lastCarouselBookIndex;
          invalidateCoverCache();
        }
        requestUpdate();
      }
    }
  } else {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto menuItems =
        buildSelectableHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings,
                                     metrics.homeContinueReadingInMenu && !recentBooks.empty());
    auto handleTouch = [&](const bool activate) {
      int touchedBookIndex = -1;
      if (activate ? mappedInput.wasCoverTapped(touchedBookIndex) : mappedInput.wasCoverTouchedDown(touchedBookIndex)) {
        if (touchedBookIndex < 0 || touchedBookIndex >= visibleBookCount) return false;
        const int previousSelectorIndex = selectorIndex;
        selectorIndex = metrics.homeContinueReadingInMenu ? 0 : touchedBookIndex;
        if (activate) {
          activateSelectedHomeItem();
        } else if (selectorIndex != previousSelectorIndex) {
          requestUpdate();
        }
        return true;
      }

      int touchedMenuIndex = -1;
      if (activate ? mappedInput.wasItemTapped(touchedMenuIndex) : mappedInput.wasItemTouchedDown(touchedMenuIndex)) {
        if (touchedMenuIndex < 0 || touchedMenuIndex >= static_cast<int>(menuItems.size())) return false;
        const int previousSelectorIndex = selectorIndex;
        selectorIndex = getHomeMenuSelectionOffset(recentBooks) + touchedMenuIndex;
        if (activate) {
          activateSelectedHomeItem();
        } else if (selectorIndex != previousSelectorIndex) {
          requestUpdate();
        }
        return true;
      }
      return false;
    };

    if (handleTouch(/*activate=*/false)) {
      return;
    }
    if (handleTouch(/*activate=*/true)) {
      return;
    }

    const int menuCount = getMenuItemCount();
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  if (getHighlightedBookIndex() != previousHighlightedBookIdx) {
    updateHighlightedBookContext();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !(isCarousel && mappedInput.hasTouchHardware())) {
    activateSelectedHomeItem();
  }
}

bool HomeActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER) {
    onFileBrowserOpen();
    return true;
  }

  if (action != CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) {
    return false;
  }

  if (quickActionsLongPowerHandled && mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    quickActionsLongPowerHandled = false;
    return true;
  }

  QuickActions::showConfiguredPopup(
      quickActionsPopup, [this] { requestUpdate(); },
      [this](const auto selectedAction) {
        if (selectedAction == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH) {
          // OptionPopup has already dismissed itself. Repaint Home before flushing
          // so the full refresh cannot preserve the popup in the panel image.
          initialFullRefresh = true;
          requestUpdate();
          return;
        }
        dispatchShortcutAction(selectedAction);
      },
      [](const auto selectedAction) {
        return isPowerButtonActionAvailableOutsideReader(selectedAction) ||
               selectedAction == CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER;
      });
  return true;
}

void HomeActivity::render(RenderLock&&) {
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto displayHomeBuffer = [this] {
    const auto refreshMode = initialFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
    initialFullRefresh = false;
    renderer.displayBuffer(refreshMode);
  };

  if (usesMinimalHomeInteraction()) {
    renderer.clearScreen();

    if (minimalMenuOpen) {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      const auto menuItems =
          buildMinimalMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings);
      GUI.drawButtonMenu(
          renderer, Rect{0, metrics.homeTopPadding, pageWidth, pageHeight - metrics.homeTopPadding},
          static_cast<int>(menuItems.size()), minimalMenuIndex,
          [&menuItems](int index) { return menuItems[index].label; },
          [&menuItems](int index) { return menuItems[index].icon; });
      if (showMinimalHomeButtonHints(mappedInput)) {
        const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT),
                                                  tr(STR_DIR_UP), tr(STR_DIR_DOWN));
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      displayHomeBuffer();
      return;
    }

    bool bufferRestored = coverBufferStored && restoreCoverBuffer();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

    coverRectX = 0;
    coverRectY = metrics.homeTopPadding;
    coverRectW = pageWidth;
    coverRectH = metrics.homeCoverTileHeight;

    GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                            recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                            std::bind(&HomeActivity::storeCoverBuffer, this),
                            hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent,
                            &globalStats, currentBookChapterTitle.c_str());

    // Minimal and Dashboard have no persistent menu to narrow, so the theme
    // only ever hands back a below-content region (or an empty one when
    // there's no safe room) -- menuRect/buttonCount/buttonLabel are unused.
    // Title/chapter are resolved with the same fallback drawRecentBookCover's
    // draw path uses (title-or-path, chapter-or-author), so a theme measuring
    // them sees exactly what will land on screen.
    const Rect minimalCoverRect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight};
    const char* companionBookTitle = nullptr;
    const char* companionChapterTitle = nullptr;
    if (!recentBooks.empty()) {
      const RecentBook& companionBook = recentBooks[0];
      companionBookTitle = companionBook.title.empty() ? companionBook.path.c_str() : companionBook.title.c_str();
      companionChapterTitle =
          !currentBookChapterTitle.empty() ? currentBookChapterTitle.c_str() : companionBook.author.c_str();
    }
    const HomeCompanionContext companionContext{companionBookTitle, companionChapterTitle,
                                                hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr,
                                                currentBookProgressPercent};
    const auto minimalCompanion =
        GUI.getHomeCompanionLayout(renderer, Rect{}, minimalCoverRect, 0, [](int) { return std::string(); },
                                   pageHeight - metrics.buttonHintsHeight, companionContext);
    companionFrame++;
    drawCompanion(minimalCompanion.region);

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }
    if (showMinimalHomeButtonHints(mappedInput)) {
      MinimalTheme::setHomeButtonHintSelection(minimalHomeNavIndex);
      GUI.drawButtonHints(renderer, tr(STR_MENU), tr(STR_BROWSE), tr(STR_SETTINGS_SHORT),
                          recentBooks.empty() ? "" : tr(STR_READ));
    }

    displayHomeBuffer();

    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
      return;
    }

    if (!recentsLoaded && !recentsLoading) {
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
    return;
  }

  // Fast path: pre-rendered frames ready — memcpy + border overlay
  if (carouselFramesReady) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int centerIdx = inCarouselRow ? selectorIndex : lastCarouselBookIndex;
    int slotIdx = gCarouselCache.findFrameSlot(centerIdx);

    if (frameBuffer && slotIdx < 0 && gCarouselCache.keyHash != 0 && bookCount > 0) {
      const int evictSlot = chooseCarouselEvictionSlot(centerIdx, bookCount);
      if (evictSlot >= 0 && loadCarouselFrameFromDisk(gCarouselCache.keyHash, bookCount, centerIdx, evictSlot)) {
        slotIdx = evictSlot;
      }
    }

    if (frameBuffer && slotIdx >= 0 && carouselFrames[slotIdx]) {
      memcpy(frameBuffer, carouselFrames[slotIdx], renderer.getBufferSize());
      LyraCarouselTheme::setPreRenderIndex(centerIdx);

      // Cached carousel frames include the header; redraw it so dynamic values
      // like battery percentage and clock are current for every restored frame.
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
      GUI.drawCarouselBorder(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                             recentBooks, centerIdx, inCarouselRow);
      const auto menuItems =
          buildHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings);
      if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
        static_cast<const LyraCarouselTheme&>(GUI).registerButtonMenuTouchTargets(renderer,
                                                                                  static_cast<int>(menuItems.size()));
      }
      if (!inCarouselRow) {
        if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) ==
            CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
          static_cast<const LyraCarouselTheme&>(GUI).drawButtonMenuSelectionOverlay(
              renderer, static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
              [&menuItems](int index) { return menuItems[index].label; },
              [&menuItems](int index) { return menuItems[index].icon; });
        }
      }

      displayHomeBuffer();
      // E-ink refresh complete — pre-render the missing adjacent frame while idle.
      updateSlidingWindowCache(centerIdx, bookCount);
      // Mirror the slow-path trigger: generate missing thumbnails on the second
      // render so the E-ink is already showing something before the SD work starts.
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      } else if (!recentsLoaded && !recentsLoading) {
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
      }
      return;
    }
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  auto menuItems =
      buildSelectableHomeMenuItems(hasOpdsServers, hasAo3Library, hasBookFusion, hasReadingStats, hasBookmarks, hasClippings,
                                   metrics.homeContinueReadingInMenu && !recentBooks.empty());
  int homeCoverTileHeight = metrics.homeCoverTileHeight;
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CLASSIC) {
    // Keep the four always-present actions clear of the button-hint strip on
    // shorter displays; any optional actions paginate below them.
    const int menuRows = std::min(4, static_cast<int>(menuItems.size()));
    const int requiredMenuHeight =
        metrics.verticalSpacing + menuRows * metrics.menuRowHeight + std::max(0, menuRows - 1) * metrics.menuSpacing;
    const int maxCoverHeight = pageHeight - metrics.buttonHintsHeight - metrics.homeTopPadding -
                               metrics.homeMenuTopOffset - requiredMenuHeight;
    homeCoverTileHeight = std::clamp(maxCoverHeight, 0, metrics.homeCoverTileHeight);
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;

  const int menuStartY = metrics.homeTopPadding + homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int menuEndY = pageHeight - metrics.buttonHintsHeight;
  const int menuHeight = std::max(0, menuEndY - menuStartY);
  Rect menuRect{0, menuStartY, pageWidth, menuHeight};
  const Rect coverRect{0, metrics.homeTopPadding, pageWidth, homeCoverTileHeight};
  const auto labelCStrAt = [&menuItems](int index) { return menuItems[index].label; };
  const auto labelAt = [&menuItems](int index) { return std::string(menuItems[index].label); };

  // The theme decides where the companion goes and what gives up room for it.
  // Deriving it here from the metrics table is only ever right for one theme:
  // each spaces its rows differently, RoundedRaff sizes them from the font, and
  // a theme whose rows are narrower than the screen has more usable space
  // beside its menu than underneath it.
  // Only Dashboard's override reads the context, and Dashboard never reaches
  // this branch (it goes through usesMinimalHomeInteraction() instead), so
  // the default (empty) context is enough here.
  const auto companion = GUI.getHomeCompanionLayout(renderer, menuRect, coverRect, static_cast<int>(menuItems.size()),
                                                    labelAt, pageHeight - metrics.buttonHintsHeight,
                                                    HomeCompanionContext{});
  if (companion.menuWidth > 0) menuRect.width = companion.menuWidth;
  companionMenuWidth = menuRect.width;

  // Narrowing the rect is all it takes to move the cover: it centres its book
  // inside whatever it is handed.
  const Rect drawnCover =
      companion.coverWidth > 0 ? Rect{coverRect.x, coverRect.y, companion.coverWidth, coverRect.height} : coverRect;
  coverRectW = drawnCover.width;
  coverRectH = drawnCover.height;
  GUI.drawRecentBookCover(renderer, drawnCover, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this),
                          hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent);

  GUI.drawButtonMenu(renderer, menuRect, static_cast<int>(menuItems.size()),
                     selectorIndex - getHomeMenuSelectionOffset(recentBooks), labelCStrAt,
                     [&menuItems](int index) { return menuItems[index].icon; });

  companionFrame++;
  drawCompanion(companion.region);

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const char* readLabel = recentBooks.empty() ? "" : tr(STR_READ);
  const auto labels = isCarouselTheme
                          ? mappedInput.mapLabels(readLabel, tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                          : mappedInput.mapLabels(readLabel, tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (panelHoldsRetainedFrame) {
    // A sleep wake leaves the sleep screen on the panel and skips the clearing
    // pass so resume stays fast, on the assumption the reader repaints next.
    // Landing on home instead, the fast waveform paints over the retained frame
    // without clearing it and the sleep screen ghosts through. HALF_REFRESH
    // requests the resync that clears it; after this the panel is ours and
    // every later paint can stay fast.
    panelHoldsRetainedFrame = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    displayHomeBuffer();
  }

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
    return;
  }

  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }

  if (carouselWarmupPending && !carouselFramesReady) {
    // Resolve any missing cover thumbs first, then warm the carousel snapshot.
    // Cover generation needs more contiguous heap than the frame cache path.
    carouselWarmupPending = false;
    const bool showedWarmupProgress = preRenderCarouselFrames(true);
    if (carouselFramesReady || showedWarmupProgress) {
      requestUpdate();
    }
  }
}

void HomeActivity::renderCarouselFrame(int bookIdx, int slotIdx) {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || !gCarouselCache.frames[slotIdx]) return;
  renderCarouselFrameToCurrentBuffer(bookIdx, nullptr, nullptr, nullptr);

  memcpy(gCarouselCache.frames[slotIdx], frameBuffer, renderer.getBufferSize());
  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
}

void HomeActivity::updateSlidingWindowCache(int centerIdx, int bookCount) {
  (void)centerIdx;
  (void)bookCount;
  // The current carousel cache keeps one frame in RAM; other frames are paged
  // from the SD snapshot cache on demand in render().
}

void HomeActivity::onSelectBook(const std::string& path) {
  gCarouselCache.invalidate();
  freeCarouselFrames();
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }
  activityManager.goToReader(path);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onContinueReading() {
  if (recentBooks.empty()) return;

  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const int bookIndex = isCarousel ? getHighlightedBookIndex() : 0;
  if (bookIndex >= 0 && bookIndex < static_cast<int>(recentBooks.size())) {
    onSelectBook(recentBooks[bookIndex].path);
  }
}

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onAo3LibraryOpen() {
  const size_t initialIndex =
      APP_STATE.ao3LibraryReturnIndex >= 0 ? static_cast<size_t>(APP_STATE.ao3LibraryReturnIndex) : 0;
  APP_STATE.ao3LibraryReturnIndex = -1;
  startActivityForResult(std::make_unique<Ao3LibraryActivity>(renderer, mappedInput, initialIndex),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::onBookFusionOpen() {
  startActivityForResult(std::make_unique<BookFusionSettingsActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::onReadingStatsOpen() {
  const int highlightedBookIdx = getHighlightedBookIndex();
  const std::string bookTitle =
      highlightedBookIdx >= 0 ? recentBooks[highlightedBookIdx].title : std::string(tr(STR_READING_STATS));
  const std::string bookPath = getCurrentBookPath();
  const std::string cachePath =
      FsHelpers::hasEpubExtension(bookPath) ? Epub::cachePathForFilePath(bookPath, "/.crosspoint") : std::string{};
  if (showAllDevicesStats) {
    startActivityForResult(std::make_unique<BookStatsActivity>(renderer, mappedInput, bookTitle, cachePath,
                                                               currentBookStats, currentBookProgressPercent, false, 0,
                                                               globalStats, allDevicesGlobalStats, true),
                           [this](const ActivityResult& result) {
                             mappedInput.suppressNextConfirmRelease();
                             const auto* statsResult = std::get_if<ReadingStatsResult>(&result.data);
                             if (statsResult && statsResult->changed) {
                               globalStats = GlobalReadingStats::load();
                               showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
                               allDevicesGlobalStats =
                                   showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
                               bookStatsCached = false;
                               updateHighlightedBookContext();
                             }
                             requestUpdate();
                           });
  } else {
    startActivityForResult(
        std::make_unique<BookStatsActivity>(renderer, mappedInput, bookTitle, cachePath, currentBookStats,
                                            currentBookProgressPercent, false, 0, globalStats, true),
        [this](const ActivityResult& result) {
          mappedInput.suppressNextConfirmRelease();
          const auto* statsResult = std::get_if<ReadingStatsResult>(&result.data);
          if (statsResult && statsResult->changed) {
            globalStats = GlobalReadingStats::load();
            showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
            allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
            bookStatsCached = false;
            updateHighlightedBookContext();
          }
          requestUpdate();
        });
  }
}

void HomeActivity::onSavedItemsOpen() {
  startActivityForResult(std::make_unique<SavedItemsHomeActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::drawCompanionColumn(const Rect region, const char* label, const char* sub, const char* quote) const {
  constexpr int PAD = 6;  // bubble inner padding
  constexpr int TAIL_LENGTH = 12;
  constexpr int BUBBLE_GAP = 2;  // between the tail tip and the character's head
  constexpr int LABEL_GAP = 2;
  constexpr int SUBLABEL_GAP = 0;
  constexpr int DESCENDER_ALLOWANCE = 3;
  constexpr int MARGIN = 4;  // keeps the block off the menu pills and the hints
  constexpr int WALK_STEPS = 6;
  constexpr int WALK_TRAVEL = 14;
  constexpr int BOB_HEIGHT = 3;
  constexpr int MAX_SCALE = 4;
  constexpr int MIN_BUBBLE_W = 90;

  const int colX = region.x + MARGIN;
  const int colW = region.width - MARGIN * 2;
  const int colTop = region.y + MARGIN;
  const int colH = region.height - MARGIN * 2;
  if (colW < MIN_BUBBLE_W) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID) + DESCENDER_ALLOWANCE;
  const int subH = renderer.getTextHeight(SMALL_FONT_ID) + DESCENDER_ALLOWANCE;
  // Small font for the quote: narrower glyphs mean fewer of the ~50-character
  // lines need a fourth row, which wrappedText's 3-line cap would otherwise
  // ellipsis away.
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int textW = colW - PAD * 2;

  // The bubble is measured before the character is sized, because the quote
  // needs however many lines it needs and the character takes what is left.
  std::vector<std::string> lines;
  if (quote) {
    if (renderer.getTextWidth(SMALL_FONT_ID, quote) <= textW) {
      lines.emplace_back(quote);
    } else {
      lines = renderer.wrappedText(SMALL_FONT_ID, quote, textW, 3);
    }
  }
  const int bubbleH = lines.empty() ? 0 : static_cast<int>(lines.size()) * lineH + PAD * 2;
  const int bubbleBlock = lines.empty() ? 0 : bubbleH + TAIL_LENGTH + BUBBLE_GAP;
  const int statusBlock = LABEL_GAP + labelH + SUBLABEL_GAP + (sub[0] != '\0' ? subH : 0);

  int scale = 0;
  for (int candidate = MAX_SCALE; candidate >= 1; candidate--) {
    if (companion::poseWidth(candidate) + WALK_TRAVEL > colW) continue;
    if (bubbleBlock + companion::poseHeight(candidate) + BOB_HEIGHT + statusBlock <= colH) {
      scale = candidate;
      break;
    }
  }
  if (scale == 0) return;

  const int spriteW = companion::poseWidth(scale);
  const int spriteH = companion::poseHeight(scale);
  const int blockH = bubbleBlock + spriteH + BOB_HEIGHT + statusBlock;
  const int blockTop = colTop + (colH - blockH) / 2;

  // Bubble spans the column; the character paces underneath it, centred on the
  // range it walks rather than on its own width so it does not appear to drift.
  if (!lines.empty()) {
    companion::drawSpeechBubble(renderer, colX, blockTop, colW, bubbleH, TAIL_LENGTH, companion::TailSide::Bottom);
    const Rect textBounds{colX + PAD, blockTop, textW, bubbleH};
    int textY = blockTop + PAD;
    for (const auto& line : lines) {
      UITheme::drawCenteredText(renderer, textBounds, SMALL_FONT_ID, textY, line.c_str());
      textY += lineH;
    }
  }

  const uint32_t phase = companionFrame % (WALK_STEPS * 2);
  const bool walkingBack = phase >= WALK_STEPS;
  const uint32_t step = walkingBack ? (WALK_STEPS * 2 - 1 - phase) : phase;
  const int walkX = static_cast<int>(step) * WALK_TRAVEL / (WALK_STEPS - 1);
  const int bob = (companionFrame % 2) ? BOB_HEIGHT : 0;

  const auto id = CompanionTracker::activeId();
  const auto mood = COMPANION.currentMood();
  const bool restless = mood != companion::Mood::Neglected;
  const int laneX = colX + (colW - spriteW - WALK_TRAVEL) / 2;
  const int spriteTop = blockTop + bubbleBlock;
  companion::drawPose(renderer, id, mood, laneX + (restless ? walkX : WALK_TRAVEL / 2),
                      restless ? spriteTop + bob : spriteTop, scale, restless && walkingBack);

  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  const int subW = sub[0] != '\0' ? renderer.getTextWidth(SMALL_FONT_ID, sub) : 0;
  const int labelY = spriteTop + spriteH + BOB_HEIGHT + LABEL_GAP;
  const int centreX = colX + colW / 2;
  renderer.drawText(UI_10_FONT_ID, centreX - labelW / 2, labelY, label, true, EpdFontFamily::BOLD);
  if (subW > 0) {
    renderer.drawText(SMALL_FONT_ID, centreX - subW / 2, labelY + labelH + SUBLABEL_GAP, sub);
  }
}
