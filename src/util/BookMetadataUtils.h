#pragma once

#include <string>
#include <vector>

// Shared, dependency-light book-metadata helpers usable from both the
// activities layer (BookActions) and the network layer (CrossPointWebServer,
// WebDAVHandler) -- neither of the latter may depend on src/activities/home,
// so this intentionally does not live in BookActions.h/.cpp.
namespace BookMetadataUtils {

// Tombstones any AO3 index record and deletes the cache dir, bookmarks, and
// clippings for a single book file. Use on delete, not on rename/move (which
// should migrate this state to the new path instead via
// BookMoveUtils::migrateMovedEpubState).
void clearFileMetadata(const std::string& fullPath);

// Recursively collects the full path of every book file (epub/xtc/txt/md)
// under dirPath that could carry metadata (bookmarks, clippings, AO3 index
// records, cache dirs) -- for use before deleting a whole folder.
void collectMetadataPathsRecursively(const std::string& dirPath, std::vector<std::string>& paths);

}  // namespace BookMetadataUtils
