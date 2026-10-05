#include "BookActions.h"

#include <Epub.h>
#include <Epub/EpubRenderMode.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "../../Ao3Librarian.h"
#include "Ao3MarkedForLaterStore.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Ao3ArchiveUtils.h"
#include "util/BookCacheUtils.h"
#include "util/BookMetadataUtils.h"
#include "util/BookMoveUtils.h"

namespace BookActions {
namespace {

bool hasReadingStats(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

std::string bookStatsCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  return "";
}

// Cheap -- getLibraryInfo only reads the small ao3_library_info sidecar via
// the cache-path hash, no epub content is loaded.
bool isAo3IndexedFic(const std::string& path) {
  if (!FsHelpers::hasEpubExtension(path)) return false;
  Ao3LibraryMetadata meta;
  return Ao3Librarian::getLibraryInfo(Epub(path, "/.crosspoint"), meta);
}

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(includeRemoveFromRecents ? 13 : 12);
  // isAo3IndexedFic() is sidecar-based (see its own comment below), so it's true whether the fic is
  // currently live-indexed or archived -- exactly the "is this an AO3 fic at all" check ToggleCompleted
  // needs here, ahead of the later archive-branch's own isArchived()-first ordering.
  const bool isAo3Fic = isAo3IndexedFic(fullPath);
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::BookInfo, StrId::STR_BOOK_INFO});
  }
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::EpubRenderMode, StrId::STR_EPUB_RENDER_MODE});
    items.push_back({FileBrowserAction::ResetReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS});
  }
  if (hasReadingStats(fullPath)) {
    if (SETTINGS.shouldTrackReadingStats()) {
      const bool bookEnabled = isBookStatsTrackingEnabled(fullPath);
      items.push_back({FileBrowserAction::ToggleBookStatsTracking, StrId::STR_TRACK_READING_STATS,
                       bookEnabled ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF});
      if (bookEnabled) {
        items.push_back({FileBrowserAction::ReadingStats, StrId::STR_READING_STATS});
        items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
      }
    }
    // AO3 fics use their own 5-state reading-status cycle (reader-only today, via CYCLE_STATUS) and
    // their own index-aware Archive Fic/Restore, neither of which this generic toggle or its
    // Archive-folder linkage (BookMoveUtils::archiveBook/restoreBook) know about -- offering it here
    // would silently leave a stale AO3 index record behind a book BookMoveUtils just moved.
    if (!isAo3Fic) {
      items.push_back({FileBrowserAction::ToggleCompleted,
                       isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
    }
  }
  // Offered from all three real callers (RecentBooksActivity, FileBrowserActivity,
  // RecentBooksGridActivity) via this one shared code path, rather than a
  // per-call-site duplicate -- pinning only makes sense for a book already
  // tracked in RecentBooksStore (it's what puts a book on the Home rail).
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto recentIt =
      std::find_if(recents.begin(), recents.end(), [&](const RecentBook& b) { return b.path == fullPath; });
  if (recentIt != recents.end()) {
    items.push_back({recentIt->pinned ? FileBrowserAction::UnpinFromHome : FileBrowserAction::PinToHome,
                     recentIt->pinned ? StrId::STR_UNPIN_FROM_HOME : StrId::STR_PIN_TO_HOME});
  }
  // isArchived() must be checked BEFORE isAo3IndexedFic(): archiving moves the
  // whole cache dir (including the sidecar isAo3IndexedFic reads) to the new
  // path, so an archived fic's sidecar is still valid there and the indexed
  // check alone would always be true, making this branch's Restore option
  // unreachable.
  if (Ao3ArchiveUtils::isArchived(fullPath)) {
    items.push_back({FileBrowserAction::RestoreFic, StrId::STR_RESTORE_TITLE});
  } else if (isAo3Fic) {
    const bool marked = AO3_MARKED_FOR_LATER_STORE.contains(fullPath);
    items.push_back({marked ? FileBrowserAction::UnmarkForLater : FileBrowserAction::MarkForLater,
                     marked ? StrId::STR_UNMARK_FOR_LATER : StrId::STR_MARK_FOR_LATER});
    items.push_back({FileBrowserAction::ArchiveFic, StrId::STR_ARCHIVE_FILE});
  } else if (BookMoveUtils::isInArchiveFolder(fullPath)) {
    // A plain (non-AO3) book currently under /Archive -- offer Restore instead of Archive File,
    // same either-or convention as the AO3 branch above.
    items.push_back({FileBrowserAction::RestoreBook, StrId::STR_RESTORE_TITLE});
  } else if (FsHelpers::hasEpubExtension(fullPath)) {
    // Independent of Mark as Finished: archiving is a standalone action, reachable any time, not
    // just as a side effect of finishing a book (matching how AO3's own Archive Fic works above).
    items.push_back({FileBrowserAction::ArchiveBook, StrId::STR_ARCHIVE_FILE});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
  }
  return items;
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

bool canSendNearby(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasXtcExtension(path) ||
         FsHelpers::hasPngExtension(path) || FsHelpers::hasBmpExtension(path);
}

void clearFileMetadata(const std::string& fullPath) { BookMetadataUtils::clearFileMetadata(fullPath); }

bool clearBookCache(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath)) {
    return clearBookCachePreservingUserState(fullPath);
  }
  return false;
}

bool deleteBookStats(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::remove(cachePath);
}

std::unique_ptr<Activity> createReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::string& fullPath, const std::string& title) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty() || !BookStatsTracking::isEnabled(cachePath)) {
    LOG_ERR("BookActions", "No reading stats for: %s", fullPath.c_str());
    return {};
  }
  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("BookActions", "Could not create stats cache for: %s", fullPath.c_str());
    return {};
  }

  const RecentBook book{fullPath, title, {}, {}};
  const float progress = FsHelpers::hasEpubExtension(fullPath) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                               : RecentBookProgress::loadPercent(book);
  const BookReadingStats stats = BookReadingStats::load(cachePath);
  const GlobalReadingStats global = GlobalReadingStats::load();
  if (GlobalReadingStats::hasSyncedStats()) {
    return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0,
                                                global, GlobalReadingStats::loadAggregated(global));
  }
  return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0,
                                              global);
}

bool resetBookReaderSettings(const std::string& fullPath) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }
  return EpubReaderActivity::resetBookReaderSettings(fullPath);
}

std::vector<std::string> epubRenderModeOptions() {
  return {I18N.get(StrId::STR_RENDER_MODE_CROSSINK_DEFAULT), I18N.get(StrId::STR_RENDER_MODE_BALANCED),
          I18N.get(StrId::STR_RENDER_MODE_LIGHT)};
}

uint8_t epubRenderModeDisplayIndex(const uint8_t renderMode) {
  switch (static_cast<EpubRenderMode>(renderMode)) {
    case EpubRenderMode::Balanced:
      return 1;
    case EpubRenderMode::Light:
      return 2;
    case EpubRenderMode::CrossInkDefault:
    default:
      return 0;
  }
}

uint8_t epubRenderModeForDisplayIndex(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return static_cast<uint8_t>(EpubRenderMode::Balanced);
    case 2:
      return static_cast<uint8_t>(EpubRenderMode::Light);
    case 0:
    default:
      return static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  }
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool completingWouldArchive(const std::string& fullPath) {
  return SETTINGS.moveFinishedToArchiveFolder && FsHelpers::hasEpubExtension(fullPath) &&
         !BookMoveUtils::isInArchiveFolder(fullPath) && !isBookCompleted(fullPath);
}

bool uncompletingWouldRestore(const std::string& fullPath) {
  return SETTINGS.moveFinishedToArchiveFolder && FsHelpers::hasEpubExtension(fullPath) &&
         BookMoveUtils::isInArchiveFolder(fullPath) && isBookCompleted(fullPath);
}

bool isBookStatsTrackingEnabled(const std::string& fullPath) {
  return BookStatsTracking::isBookEnabled(bookStatsCachePath(fullPath));
}

bool toggleBookStatsTracking(const std::string& fullPath, bool& enabled) {
  if (!hasReadingStats(fullPath) || !SETTINGS.shouldTrackReadingStats()) return false;
  const std::string cachePath = bookStatsCachePath(fullPath);
  enabled = !BookStatsTracking::isBookEnabled(cachePath);
  if (BookStatsTracking::setBookEnabled(cachePath, enabled)) return true;
  enabled = BookStatsTracking::isBookEnabled(cachePath);
  return false;
}

bool setBookCompletedOnDisk(const std::string& fullPath, const bool completed) {
  // setupCacheDir() (not just bookStatsCachePath()'s bare path computation) because this may be the
  // first time this book has ever had state written for it -- e.g. finishing a book from the File
  // Browser without ever having opened it in the reader.
  std::string cachePath;
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub epub(fullPath, "/.crosspoint");
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc xtc(fullPath, "/.crosspoint");
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
  } else {
    return false;
  }

  BookReadingStats stats = BookReadingStats::load(cachePath);
  if (stats.isCompleted == completed) return true;

  stats.isCompleted = completed;
  const bool trackStats = BookStatsTracking::isEnabled(cachePath);
  if (completed && trackStats && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  if (!stats.save(cachePath)) {
    LOG_ERR("BookActions", "Could not save completion for: %s", fullPath.c_str());
    return false;
  }
  if (trackStats) {
    GlobalReadingStats globalStats = GlobalReadingStats::load();
    if (completed) {
      globalStats.completedBooks++;
    } else if (globalStats.completedBooks > 0) {
      globalStats.completedBooks--;
    }
    globalStats.save();
  }

  // Changing completion status does not open a book. The reader adds it to
  // recents if it is opened again after being marked unfinished.
  if (SETTINGS.removeReadBooksFromRecents && completed) RECENT_BOOKS.removeByPath(fullPath);

  return true;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed,
                         const bool allowMove) {
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  if (!isEpub && !isXtc) {
    return false;
  }
  if (isXtc && !Xtc(fullPath, "/.crosspoint").load()) {
    return false;
  }

  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) return false;
  completed = !BookReadingStats::load(cachePath).isCompleted;

  if (!setBookCompletedOnDisk(fullPath, completed)) {
    return false;
  }

  // Two-way sync with the Archive folder, both gated by the same setting: finishing an epub not yet
  // archived offers to move it in; un-finishing one already archived offers to move it back out. Each
  // direction goes through the same archiveBook()/restoreBook() the standalone Archive File/Restore
  // actions use, so a finish-triggered archive still leaves the restore marker Restore depends on.
  // Never for an AO3 fic: buildBookActionItems() never offers ToggleCompleted for one (only its own
  // index-aware Archive Fic/Restore, or the reader's Cycle Status), but this guards against some future
  // caller reaching toggleBookCompleted() directly on an AO3 path, which would otherwise silently leave a
  // stale AO3 index record behind a move BookMoveUtils doesn't know how to keep that index in sync with.
  if (allowMove && isEpub && SETTINGS.moveFinishedToArchiveFolder && !isAo3IndexedFic(fullPath)) {
    if (completed && !BookMoveUtils::isInArchiveFolder(fullPath)) {
      LOG_INF("BookActions", "Moving completed epub: %s", fullPath.c_str());
      if (BookMoveUtils::archiveBook(fullPath).empty()) {
        LOG_ERR("BookActions", "Failed to move book to Archive folder");
        snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_ARCHIVE_FAILED_TITLE));
        snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_ARCHIVE_FAILED_BODY),
                 displayName.c_str());
        APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      }
    } else if (!completed && BookMoveUtils::isInArchiveFolder(fullPath)) {
      LOG_INF("BookActions", "Restoring unfinished epub: %s", fullPath.c_str());
      if (BookMoveUtils::restoreBook(fullPath).empty()) {
        LOG_ERR("BookActions", "Failed to restore book from Archive folder");
        snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_RESTORE_FAILED_TITLE));
        snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_RESTORE_FAILED_BODY),
                 displayName.c_str());
        APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      }
    }
  } else if (allowMove && isEpub && completed && SETTINGS.moveFinishedToReadFolder &&
            !BookMoveUtils::isInArchiveFolder(fullPath) && fullPath.rfind("/Read/", 0) != 0) {
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!Storage.rename(fullPath.c_str(), dstPath.c_str())) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    }
  }

  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
}

}  // namespace BookActions
