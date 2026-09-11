#include "BookMetadataUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include "../Ao3Librarian.h"
#include "../BookmarkStore.h"
#include "../ClippingStore.h"

namespace BookMetadataUtils {
namespace {

std::string buildFullPath(std::string basepath, const std::string& entry) {
  if (basepath.empty() || basepath.back() != '/') basepath += "/";
  return basepath + entry;
}

bool hasFileMetadata(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

}  // namespace

void clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    // Tombstone any live AO3 index record before the cache dir (which holds
    // its sidecar) is wiped below -- otherwise a fic deleted from here
    // (rather than from the AO3 Library's own delete path) leaves a
    // permanent ghost row in the AO3 Library/Dashboard, since nothing else
    // ever runs Ao3Librarian::sanitizeIndex() automatically. A no-op for a
    // non-AO3 epub (no matching hash) or on a branch with no AO3 index file.
    Ao3Librarian::tombstoneRecord(fullPath);
    Epub(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
    ClippingStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  }
}

void collectMetadataPathsRecursively(const std::string& dirPath, std::vector<std::string>& paths) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("BookMeta", "Failed to scan directory metadata before delete: %s", dirPath.c_str());
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

}  // namespace BookMetadataUtils
