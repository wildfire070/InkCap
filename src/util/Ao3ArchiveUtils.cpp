#include "Ao3ArchiveUtils.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include "../Ao3Librarian.h"
#include "../Ao3MarkedForLaterStore.h"
#include "../Ao3NewChaptersStore.h"
#include "../Ao3WipsStore.h"
#include "BookMoveUtils.h"

namespace Ao3ArchiveUtils {

std::string archiveRoot() {
  const char* path = "/.crosspoint/ao3_settings.json";
  if (!Storage.exists(path)) return DEFAULT_ARCHIVE_ROOT;
  String json = Storage.readFile(path);
  if (json.isEmpty()) return DEFAULT_ARCHIVE_ROOT;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return DEFAULT_ARCHIVE_ROOT;
  const char* configured = doc["archiveFolderName"] | "";
  return configured[0] != '\0' ? std::string(configured) : std::string(DEFAULT_ARCHIVE_ROOT);
}

bool isArchived(const std::string& path) {
  // Path-independent by design: archiving tombstones the OLD path's index
  // record and deliberately never writes a new one at the new path, so a
  // valid sidecar with no LIVE record at the current path IS the definition
  // of "archived" -- robust even if the user renames the Archive Folder
  // setting after already archiving fics (a plain path-prefix check isn't).
  if (!FsHelpers::hasEpubExtension(path)) return false;
  Ao3LibraryMetadata meta;
  const Epub epub(path, "/.crosspoint");
  if (!Ao3Librarian::getLibraryInfo(epub, meta)) return false;
  return !Ao3Librarian::hasLiveIndexRecord(path);
}

std::string buildArchiveDestination(const std::string& srcPath) {
  const std::string root = archiveRoot();
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(root.c_str());
  std::string dstPath = root + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = root + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

std::string archiveFic(const std::string& srcPath, const std::string& title, const std::string& author) {
  const std::string oldCachePath = Epub::cachePathForFilePath(srcPath, "/.crosspoint");
  const std::string dstPath = buildArchiveDestination(srcPath);

  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("Ao3Archive", "Failed to move %s -> %s", srcPath.c_str(), dstPath.c_str());
    return "";
  }

  // Hashes srcPath itself to find the record, so this must run with the OLD
  // path, before any cache-dir renaming below.
  Ao3Librarian::tombstoneRecord(srcPath);

  if (!BookMoveUtils::migrateMovedEpubState(srcPath, dstPath, oldCachePath, title, author, /*keepInRecents=*/true)) {
    LOG_ERR("Ao3Archive", "Partial failure migrating state for %s -> %s (non-fatal)", srcPath.c_str(),
            dstPath.c_str());
  }

  AO3_MARKED_FOR_LATER_STORE.removeByPath(srcPath);
  AO3_NEW_CHAPTERS_STORE.removeByPath(srcPath);
  AO3_WIPS_STORE.removeByPath(srcPath);

  return dstPath;
}

std::string restoreFic(const std::string& archivedPath) {
  Ao3LibraryMetadata meta;
  const Epub archivedEpub(archivedPath, "/.crosspoint");
  if (!Ao3Librarian::getLibraryInfo(archivedEpub, meta) || meta.filepath[0] == '\0') {
    LOG_ERR("Ao3Archive", "No sidecar/original path found for archived fic: %s", archivedPath.c_str());
    return "";
  }

  const std::string oldCachePath = archivedEpub.getCachePath();
  std::string restoredPath = meta.filepath;
  if (Storage.exists(restoredPath.c_str())) {
    // Something already occupies the original path (e.g. a re-download) --
    // dedupe the same way a fresh archive would, rather than overwrite it.
    const size_t lastSlash = restoredPath.rfind('/');
    const std::string dir = (lastSlash != std::string::npos) ? restoredPath.substr(0, lastSlash) : "";
    const std::string filename = (lastSlash != std::string::npos) ? restoredPath.substr(lastSlash + 1) : restoredPath;
    const size_t dotPos = filename.rfind('.');
    const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
    const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
    int suffix = 2;
    do {
      restoredPath = dir + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
      suffix++;
    } while (Storage.exists(restoredPath.c_str()) && suffix < 100);
  }

  if (!Storage.rename(archivedPath.c_str(), restoredPath.c_str())) {
    LOG_ERR("Ao3Archive", "Failed to move %s -> %s", archivedPath.c_str(), restoredPath.c_str());
    return "";
  }

  if (!BookMoveUtils::migrateMovedEpubState(archivedPath, restoredPath, oldCachePath, meta.title, meta.author,
                                            /*keepInRecents=*/true)) {
    LOG_ERR("Ao3Archive", "Partial failure migrating state for %s -> %s (non-fatal)", archivedPath.c_str(),
            restoredPath.c_str());
  }

  // Recreates a live index record at the restored path's hash; the
  // tombstoned slot left by archiveFic() is treated as free, not matched by
  // cacheHash, so this lands cleanly with no manual un-tombstone step.
  const Epub restoredEpub(restoredPath, "/.crosspoint");
  Ao3Librarian::scrape(restoredEpub, /*force=*/true);

  return restoredPath;
}

}  // namespace Ao3ArchiveUtils
