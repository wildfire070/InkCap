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
constexpr char READ_FOLDER[] = "/Read";

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
}  // namespace

namespace BookMoveUtils {

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
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
  bool bookmarksTouched = false;
  bool clippingsTouched = false;
  bool recentMoved = false;

  auto recover = [&]() {
    bool rolledBack = true;
    if (recentMoved && !RECENT_BOOKS.updatePath(newPath, oldPath, newCachePath, oldCachePath)) {
      rolledBack = false;
    }
    if (clippingsTouched && !ClippingStore::migrateForFilePath(newPath, oldPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to roll back clipping migration %s -> %s", newPath.c_str(), oldPath.c_str());
      rolledBack = false;
    }
    if (bookmarksTouched && !BookmarkStore::migrateForFilePath(newPath, oldPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to roll back bookmark migration %s -> %s", newPath.c_str(), oldPath.c_str());
      rolledBack = false;
    }
    if (cacheMoved && Storage.exists(newCachePath.c_str()) &&
        !Storage.rename(newCachePath.c_str(), oldCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to roll back cache migration %s -> %s", newCachePath.c_str(), oldCachePath.c_str());
      rolledBack = false;
    }
    if (rolledBack) return RenameMigrationResult::RolledBack;

    LOG_ERR("BookMove", "State rollback was incomplete; preserving the new book path");
    if (cacheMoved && Storage.exists(oldCachePath.c_str()) &&
        !Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to recover cache at new path %s", newCachePath.c_str());
    }
    if (!BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to recover bookmarks at new path %s", newPath.c_str());
    }
    if (strcmp(bookType, "epub") == 0 &&
        !ClippingStore::migrateForFilePath(oldPath, newPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to recover clippings at new path %s", newPath.c_str());
    }
    if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
      LOG_ERR("BookMove", "Failed to recover recent book at new path %s", newPath.c_str());
    }
    if (APP_STATE.openEpubPath == oldPath) {
      APP_STATE.openEpubPath = newPath;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BookMove", "Failed to recover open book at new path %s", newPath.c_str());
      }
    }
    return RenameMigrationResult::KeepRenamed;
  };

  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s", oldCachePath.c_str(), newCachePath.c_str());
      return RenameMigrationResult::RolledBack;
    }
    cacheMoved = true;
  }

  bookmarksTouched = true;
  if (!BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, bookType)) {
    LOG_ERR("BookMove", "Failed to migrate bookmarks for renamed book %s -> %s", oldPath.c_str(), newPath.c_str());
    return recover();
  }

  if (strcmp(bookType, "epub") == 0) {
    clippingsTouched = true;
    if (!ClippingStore::migrateForFilePath(oldPath, newPath, title, author, bookType)) {
      LOG_ERR("BookMove", "Failed to migrate clippings for renamed book %s -> %s", oldPath.c_str(), newPath.c_str());
      return recover();
    }
  }

  if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
    return recover();
  }
  recentMoved = true;

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("BookMove", "Failed to save renamed open book path: %s", newPath.c_str());
      APP_STATE.openEpubPath = oldPath;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BookMove", "Failed to restore open book path after rename failure: %s", oldPath.c_str());
        APP_STATE.openEpubPath = newPath;
        if (!APP_STATE.saveToFile()) {
          LOG_ERR("BookMove", "Failed to recover open book at new path %s", newPath.c_str());
        }
        return RenameMigrationResult::KeepRenamed;
      }
      return recover();
    }
  }

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
      // Reading progress and (for AO3) the library sidecar live in this
      // cache dir -- bail before touching bookmarks/clippings/recents below
      // so the caller can still roll back the epub file's own rename and
      // keep the book at one consistent location, rather than orphaning
      // this state under a path nothing points at anymore.
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
