#include "BookMoveUtils.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
bool getCachePath(const std::string& bookPath, const char* bookType, std::string& cachePath) {
  if (strcmp(bookType, "epub") == 0) {
    cachePath = Epub::cachePathForFilePath(bookPath, "/.crosspoint");
  } else if (strcmp(bookType, "xtc") == 0) {
    cachePath = Xtc(bookPath, "/.crosspoint").getCachePath();
  } else if (strcmp(bookType, "txt") == 0) {
    cachePath = Txt(bookPath, "/.crosspoint").getCachePath();
  } else {
    LOG_ERR("BookMove", "Unknown book type for state migration: %s", bookType);
    return false;
  }
  return true;
}

// Marker recording a book's pre-archive path, written into its OLD cache dir by archiveBook() so it
// rides along (like Ao3ArchiveUtils::archiveFic's own "marked_for_later" marker) to the new cache dir
// once migrateMovedEpubState renames it. Lets restoreBook() find its way back with no index to
// consult -- a plain book has none, unlike AO3's tombstoned-and-rescraped record.
constexpr char ARCHIVED_FROM_MARKER[] = "archived_from";

// Appends the destination's own directory, preserving its filename, dedupes with " (2)", " (3)", ...
// on a same-name collision. Shared by every path that computes where an archived or restored book
// should land.
std::string dedupedDestination(const std::string& desiredPath) {
  if (!Storage.exists(desiredPath.c_str())) return desiredPath;

  const size_t lastSlash = desiredPath.rfind('/');
  const std::string dir = (lastSlash != std::string::npos) ? desiredPath.substr(0, lastSlash) : "";
  const std::string filename = (lastSlash != std::string::npos) ? desiredPath.substr(lastSlash + 1) : desiredPath;
  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";

  std::string candidate;
  int suffix = 2;
  do {
    candidate = dir + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(candidate.c_str()) && suffix < 100);
  return candidate;
}
}  // namespace

namespace BookMoveUtils {

bool isInArchiveFolder(const std::string& path) {
  constexpr size_t n = sizeof(ARCHIVE_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, ARCHIVE_FOLDER) == 0 && path[n] == '/';
}

std::string buildArchiveDestination(const std::string& srcPath) {
  Storage.mkdir(ARCHIVE_FOLDER);
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;
  return dedupedDestination(std::string(ARCHIVE_FOLDER) + "/" + filename);
}

std::string archiveBook(const std::string& srcPath) {
  const std::string oldCachePath = Epub::cachePathForFilePath(srcPath, "/.crosspoint");
  const std::string dstPath = buildArchiveDestination(srcPath);

  Epub epub(srcPath, "/.crosspoint");
  const std::string title = epub.getTitle();
  const std::string author = epub.getAuthor();

  // Written before the move so it rides along with the cache dir rename below.
  Storage.writeFile((oldCachePath + "/" + ARCHIVED_FROM_MARKER).c_str(), srcPath.c_str());

  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("BookMove", "Failed to archive %s -> %s", srcPath.c_str(), dstPath.c_str());
    Storage.remove((oldCachePath + "/" + ARCHIVED_FROM_MARKER).c_str());
    return "";
  }

  if (!migrateMovedEpubState(srcPath, dstPath, oldCachePath, title, author, /*keepInRecents=*/true)) {
    LOG_ERR("BookMove", "State migration failed for %s -> %s, rolling back", srcPath.c_str(), dstPath.c_str());
    if (!Storage.rename(dstPath.c_str(), srcPath.c_str())) {
      LOG_ERR("BookMove", "Rollback rename also failed for %s -> %s; book may be stranded at %s", dstPath.c_str(),
              srcPath.c_str(), dstPath.c_str());
    }
    return "";
  }

  return dstPath;
}

std::string restoreBook(const std::string& archivedPath) {
  const std::string archivedCachePath = Epub::cachePathForFilePath(archivedPath, "/.crosspoint");
  const std::string markerPath = archivedCachePath + "/" + ARCHIVED_FROM_MARKER;
  if (!Storage.exists(markerPath.c_str())) {
    LOG_ERR("BookMove", "No archived-from marker for: %s", archivedPath.c_str());
    return "";
  }
  const std::string originalPath = Storage.readFile(markerPath.c_str()).c_str();
  if (originalPath.empty()) {
    LOG_ERR("BookMove", "Empty archived-from marker for: %s", archivedPath.c_str());
    return "";
  }

  const std::string restoredPath = dedupedDestination(originalPath);
  Epub archivedEpub(archivedPath, "/.crosspoint");
  const std::string title = archivedEpub.getTitle();
  const std::string author = archivedEpub.getAuthor();

  if (!Storage.rename(archivedPath.c_str(), restoredPath.c_str())) {
    LOG_ERR("BookMove", "Failed to restore %s -> %s", archivedPath.c_str(), restoredPath.c_str());
    return "";
  }

  if (!migrateMovedEpubState(archivedPath, restoredPath, archivedCachePath, title, author, /*keepInRecents=*/true)) {
    LOG_ERR("BookMove", "State migration failed for %s -> %s, rolling back", archivedPath.c_str(),
            restoredPath.c_str());
    if (!Storage.rename(restoredPath.c_str(), archivedPath.c_str())) {
      LOG_ERR("BookMove", "Rollback rename also failed for %s -> %s; book may be stranded at %s", restoredPath.c_str(),
              archivedPath.c_str(), restoredPath.c_str());
    }
    return "";
  }

  // The marker rode along to the restored cache dir with the rest of the state; its job is done, so
  // remove it -- a stale marker pointing at a location the book no longer occupies would misdirect a
  // future restore if the book were somehow archived again without archiveBook() rewriting it first.
  Storage.remove((Epub::cachePathForFilePath(restoredPath, "/.crosspoint") + "/" + ARCHIVED_FROM_MARKER).c_str());
  return restoredPath;
}

RenameMigrationResult migrateRenamedBookState(const std::string& oldPath, const std::string& newPath,
                                              const std::string& oldCachePath, const std::string& title,
                                              const std::string& author, const char* bookType) {
  if (!bookType) {
    LOG_ERR("BookMove", "Missing book type for state migration");
    return RenameMigrationResult::RolledBack;
  }

  std::string newCachePath;
  if (!getCachePath(newPath, bookType, newCachePath)) return RenameMigrationResult::RolledBack;

  bool cacheMoved = false;
  bool bookRenamed = false;
  bool recentMoved = false;
  bool openPathMoved = false;
  const bool shouldMoveOpenPath = APP_STATE.openEpubPath == oldPath;
  BookmarkStore::RenameMigration bookmarkMigration;
  ClippingStore::RenameMigration clippingMigration;

  const auto commitMetadata = [&]() {
    if (!BookmarkStore::commitRenameMigration(bookmarkMigration)) {
      LOG_ERR("BookMove", "Renamed book kept stale bookmark rollback files: %s", newPath.c_str());
    }
    if (!ClippingStore::commitRenameMigration(clippingMigration)) {
      LOG_ERR("BookMove", "Renamed book kept stale clipping rollback files: %s", newPath.c_str());
    }
  };

  const auto rollbackMovedReferences = [&]() {
    bool rolledBack = true;
    if (openPathMoved) {
      APP_STATE.openEpubPath = oldPath;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BookMove", "Failed to restore open book path after rename: %s", oldPath.c_str());
        rolledBack = false;
      } else {
        openPathMoved = false;
      }
    }
    if (recentMoved) {
      if (!RECENT_BOOKS.updatePath(newPath, oldPath, newCachePath, oldCachePath)) {
        rolledBack = false;
      } else {
        recentMoved = false;
      }
    }
    return rolledBack;
  };

  const auto rollbackPreparedStorage = [&]() {
    bool rolledBack = true;
    if (!ClippingStore::rollbackRenameMigration(clippingMigration)) {
      LOG_ERR("BookMove", "Failed to restore clipping metadata after rename: %s", newPath.c_str());
      rolledBack = false;
    }
    if (!BookmarkStore::rollbackRenameMigration(bookmarkMigration)) {
      LOG_ERR("BookMove", "Failed to restore bookmark metadata after rename: %s", newPath.c_str());
      rolledBack = false;
    }
    if (cacheMoved && Storage.exists(newCachePath.c_str()) &&
        !Storage.rename(newCachePath.c_str(), oldCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to roll back cache migration %s -> %s", newCachePath.c_str(), oldCachePath.c_str());
      rolledBack = false;
    }
    return rolledBack;
  };

  const auto keepRenamed = [&]() {
    LOG_ERR("BookMove", "Could not restore the original filename; preserving state at the new path");
    // Both calls are idempotent when that reference is already at newPath,
    // and repair a partially completed rollback when it is still at oldPath.
    if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
      LOG_ERR("BookMove", "Failed to recover recent book at new path %s", newPath.c_str());
    }
    if (shouldMoveOpenPath) {
      APP_STATE.openEpubPath = newPath;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BookMove", "Failed to recover open book at new path %s", newPath.c_str());
      }
    }
    commitMetadata();
    return RenameMigrationResult::KeepRenamed;
  };

  const auto recover = [&]() {
    if (bookRenamed) {
      if (!Storage.rename(newPath.c_str(), oldPath.c_str())) {
        return keepRenamed();
      }
      bookRenamed = false;

      if (!rollbackMovedReferences()) {
        if (Storage.rename(oldPath.c_str(), newPath.c_str())) {
          bookRenamed = true;
          return keepRenamed();
        }
        LOG_ERR("BookMove", "Failed to re-establish new book path after incomplete state rollback: %s",
                newPath.c_str());
      }
    }

    if (!rollbackPreparedStorage()) {
      LOG_ERR("BookMove", "Storage rollback was incomplete; the original book path remains available");
    }
    return RenameMigrationResult::RolledBack;
  };

  // Publish the new-path metadata before the book itself is renamed. Until
  // the physical rename succeeds, the old book and its source metadata remain
  // intact; after it succeeds, the new-path metadata is already discoverable.
  if (!BookmarkStore::beginRenameMigration(oldPath, newPath, title, author, bookType, bookmarkMigration)) {
    LOG_ERR("BookMove", "Failed to migrate bookmarks for renamed book %s -> %s", oldPath.c_str(), newPath.c_str());
    return recover();
  }

  if (strcmp(bookType, "epub") == 0) {
    if (!ClippingStore::beginRenameMigration(oldPath, newPath, title, author, bookType, clippingMigration)) {
      LOG_ERR("BookMove", "Failed to migrate clippings for renamed book %s -> %s", oldPath.c_str(), newPath.c_str());
      return recover();
    }
  }

  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s", oldCachePath.c_str(), newCachePath.c_str());
      return recover();
    }
    cacheMoved = true;
  }

  if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
    LOG_ERR("BookMove", "Failed to rename file: %s -> %s", oldPath.c_str(), newPath.c_str());
    return recover();
  }
  bookRenamed = true;

  if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
    return recover();
  }
  recentMoved = true;

  if (shouldMoveOpenPath) {
    APP_STATE.openEpubPath = newPath;
    openPathMoved = true;
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("BookMove", "Failed to save renamed open book path: %s", newPath.c_str());
      return recover();
    }
  }

  commitMetadata();

  return RenameMigrationResult::Success;
}

bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, const bool keepInRecents) {
  const std::string newCachePath = Epub::cachePathForFilePath(newPath, "/.crosspoint");
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    bool cacheDirMoved = Storage.rename(oldCachePath.c_str(), newCachePath.c_str());
    if (!cacheDirMoved) {
      // A rename failing immediately after the epub file's own rename just
      // succeeded on the same filesystem is almost always transient SD I/O
      // contention -- one retry clears it in practice.
      delay(50);
      cacheDirMoved = Storage.rename(oldCachePath.c_str(), newCachePath.c_str());
    }
    if (!cacheDirMoved) {
      // Reading progress lives in this cache dir -- bail before touching
      // bookmarks/clippings/recents below so the caller can still roll back
      // the epub file's own rename and keep the book at one consistent
      // location, rather than orphaning this state under a path nothing
      // points at anymore.
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s (after retry)", oldCachePath.c_str(),
              newCachePath.c_str());
      return false;
    }
  }

  // Bookmarks/clippings are independently path-keyed stores, not part of the
  // cache dir above -- a failure here strands old-path entries rather than
  // orphaning data, so it's logged but doesn't fail the whole migration; no
  // caller has ever needed to roll back for this specifically.
  if (!BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, "epub")) {
    LOG_ERR("BookMove", "Failed to migrate bookmarks for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
  }

  if (!ClippingStore::migrateForFilePath(oldPath, newPath, title, author, "epub")) {
    LOG_ERR("BookMove", "Failed to migrate clippings for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
  }

  if (keepInRecents) {
    (void)RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  } else {
    RECENT_BOOKS.removeByPath(oldPath);
    RECENT_BOOKS.removeByPath(newPath);
  }

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }

  return true;
}

}  // namespace BookMoveUtils
