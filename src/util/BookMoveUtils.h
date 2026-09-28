#pragma once

#include <string>

namespace BookMoveUtils {

// SD card folder every finished book (AO3 or plain) is moved into. Single source of truth for the
// path -- Ao3ArchiveUtils uses this same folder for AO3 fics, rather than its own configurable root.
inline constexpr char ARCHIVE_FOLDER[] = "/Archive";

enum class RenameMigrationResult {
  Success,
  RolledBack,
  KeepRenamed,
};

// True if path is inside ARCHIVE_FOLDER (starts with "<ARCHIVE_FOLDER>/"). Non-allocating so it is
// cheap to call from loop().
bool isInArchiveFolder(const std::string& path);

// Computes the destination path under ARCHIVE_FOLDER, preserving the filename only (not any parent
// folders). Dedupes with " (2)", " (3)", ... on a same-name collision. Creates ARCHIVE_FOLDER itself
// if it doesn't exist yet. Shared by every path that archives a book, AO3 or not.
std::string buildArchiveDestination(const std::string& srcPath);
// Prepares reader metadata, renames the physical book, then commits the state
// migration so one canonical metadata path is always available across resets.
RenameMigrationResult migrateRenamedBookState(const std::string& oldPath, const std::string& newPath,
                                              const std::string& oldCachePath, const std::string& title,
                                              const std::string& author, const char* bookType);
bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, bool keepInRecents);

}  // namespace BookMoveUtils
