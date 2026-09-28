#include "BookMetadataUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

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

// collectMetadataPathsRecursively() below grows `paths` once per book file anywhere in a whole
// directory subtree (a web/WebDAV move or delete of a large folder), with no upper bound. InkCap
// builds with -fno-exceptions, so a failed push_back's std::bad_alloc has nothing to catch it and
// aborts the whole firmware (same crash class as SdCardFontRegistry/SettingsList/
// FontSelectionActivity/BmpViewerActivity's sibling-image scan/FolderPickerActivity's directory scan/
// DictionaryRegistry's dictionary scan). Stop collecting once heap gets tight rather than risk it -- a
// truncated list just means some of the moved/deleted books' metadata doesn't migrate/clear, a far
// better outcome than a hard crash.
constexpr uint32_t COLLECT_METADATA_PATHS_MIN_FREE_HEAP = 24576;
constexpr uint32_t COLLECT_METADATA_PATHS_MIN_MAX_ALLOC_HEAP = 16384;

}  // namespace

void clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
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
      if (ESP.getFreeHeap() < COLLECT_METADATA_PATHS_MIN_FREE_HEAP ||
          ESP.getMaxAllocHeap() < COLLECT_METADATA_PATHS_MIN_MAX_ALLOC_HEAP) {
        LOG_ERR("BookMeta", "Stopping metadata scan early: %u free (need %u), %u max alloc (need %u)",
                ESP.getFreeHeap(), COLLECT_METADATA_PATHS_MIN_FREE_HEAP, ESP.getMaxAllocHeap(),
                COLLECT_METADATA_PATHS_MIN_MAX_ALLOC_HEAP);
        file.close();
        dir.close();
        return;
      }
      paths.push_back(childPath);
    }
    file.close();
  }
  dir.close();
}

}  // namespace BookMetadataUtils
