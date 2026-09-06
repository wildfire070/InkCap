#pragma once

#include <string>

// Moves a finished AO3 fic out of the active library into a mirrored /AO3Read
// folder (preserving any subfolder structure under the source path) without
// losing its cache, bookmarks, or clippings -- and can move it back later.
// Pairs with the general, non-AO3 src/util/BookMoveUtils.h, whose
// migrateMovedEpubState() this reuses for the actual cache/bookmark/clipping
// migration; the AO3-specific piece here is keeping Ao3Librarian's index in
// sync with the move.
namespace Ao3ArchiveUtils {

// Fallback destination root when the user hasn't set a custom one via
// Ao3LibrarySettingsActivity's "Archive Folder" row.
inline constexpr char DEFAULT_ARCHIVE_ROOT[] = "/AO3Read";

// The user's configured archive root (the "archiveFolderName" field in
// /.crosspoint/ao3_settings.json, the same ad-hoc file "ao3Folder" already
// lives in), or DEFAULT_ARCHIVE_ROOT if unset.
std::string archiveRoot();

// True if path already lives under the current archive root.
bool isArchived(const std::string& path);

// Computes the mirrored destination path under the current archive root, preserving the
// filename and any parent-folder structure the source had beyond its last
// path component's containing folder. Dedupes with " (2)", " (3)", ... on a
// same-name collision (matching BookMoveUtils::buildReadFolderDestination).
// Creates the destination's parent directory.
std::string buildArchiveDestination(const std::string& srcPath);

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
