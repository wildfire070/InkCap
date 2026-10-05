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
  InvalidBookType,
  DestinationStateExists,
};

// True if path is inside ARCHIVE_FOLDER (starts with "<ARCHIVE_FOLDER>/"). Non-allocating so it is
// cheap to call from loop().
bool isInArchiveFolder(const std::string& path);

// Computes the destination path under ARCHIVE_FOLDER, preserving the filename only (not any parent
// folders). Dedupes with " (2)", " (3)", ... on a same-name collision. Creates ARCHIVE_FOLDER itself
// if it doesn't exist yet. Shared by every path that archives a book, AO3 or not.
std::string buildArchiveDestination(const std::string& srcPath);

std::string buildReadFolderDestination(const std::string& srcPath);
// Renames a file and migrates supported books' path-based reader state. Book
// renames must keep the same reader format. Other files also keep pinned image
// references, with rollback if those references cannot be saved.
RenameMigrationResult renameFilePreservingBookState(const std::string& oldPath, const std::string& newPath);
// Prepares reader metadata, renames the physical book, then commits the state
// migration so one canonical metadata path is always available across resets.
RenameMigrationResult migrateRenamedBookState(const std::string& oldPath, const std::string& newPath,
                                              const std::string& oldCachePath, const std::string& title,
                                              const std::string& author, const char* bookType);
bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, bool keepInRecents);

// Archives any epub (not just an AO3 fic -- see Ao3ArchiveUtils::archiveFic for that): moves it into
// ARCHIVE_FOLDER and re-keys its cache dir and migrated state (bookmarks/clippings/recents), same as
// the automatic move-on-finish. Also records the pre-archive path in the cache dir so restoreBook()
// can find its way back later -- a plain book has no index to consult for that, unlike AO3's
// tombstoned-and-rescraped record. Returns the new path on success, empty string on failure.
std::string archiveBook(const std::string& srcPath);

// Reverses archiveBook(): reads the marker archiveBook() left behind to recover the original path,
// moves the file back (deduping if something now occupies that path), and migrates state back.
// Returns the restored path on success, empty string on failure (including when archivedPath was
// never archived by archiveBook() itself, so no marker exists to restore from).
std::string restoreBook(const std::string& archivedPath);

}  // namespace BookMoveUtils
