#pragma once

#include <string>

// Moves a finished AO3 fic out of the active library into BookMoveUtils::ARCHIVE_FOLDER (the same
// fixed /Archive/ folder every other finished book moves into) without losing its cache, bookmarks,
// or clippings -- and can move it back later. Pairs with the general, non-AO3
// src/util/BookMoveUtils.h, whose migrateMovedEpubState() and buildArchiveDestination() this reuses
// for the actual move and cache/bookmark/clipping migration; the AO3-specific piece here is keeping
// Ao3Librarian's index in sync with the move.
namespace Ao3ArchiveUtils {

// True if path is an archived AO3 fic: its own sidecar exists but the index has no live record for
// it. Independent of which folder the file actually sits in -- archiving tombstones the OLD path's
// index record and deliberately never writes a new one at the new path, so a valid sidecar with no
// live record IS the definition of "archived" (a plain path-prefix check against ARCHIVE_FOLDER
// isn't, since restoreFic() can leave a fic wherever its original path was).
bool isArchived(const std::string& path);

// Archives an AO3-indexed fic: moves the file, re-keys its cache dir and
// migrated state (bookmarks/clippings/recents) via BookMoveUtils, and
// tombstones its OLD Ao3Librarian index record. Deliberately does NOT write
// a new index record or touch the sidecar's own stored filepath field --
// that staleness is what lets restoreFic() find its way back later, and a
// tombstoned record is skipped entirely by Ao3Librarian::sanitizeIndex(), so
// the mismatch is never misread as a ghost. Returns the new path on success,
// empty string on failure.
std::string archiveFic(const std::string& srcPath, const std::string& title, const std::string& author);

// Reverses archiveFic(): reads the archived fic's own sidecar (still pointing
// at the pre-archive path) to recover where it came from, moves the file
// back (deduping if something now occupies that path), migrates state back,
// and re-scrapes to recreate a live index record. Returns the restored path
// on success, empty string on failure.
std::string restoreFic(const std::string& archivedPath);

}  // namespace Ao3ArchiveUtils
