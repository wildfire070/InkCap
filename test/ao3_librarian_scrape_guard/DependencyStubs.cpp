// Real implementation of the one BookMoveUtils function Ao3Librarian.cpp's guard depends on --
// isInArchiveFolder() is a trivial, dependency-free path check (just ARCHIVE_FOLDER from the
// header + <string>), so this is a verbatim copy of the production algorithm, not a fake. The
// real BookMoveUtils.cpp is NOT linked here: it pulls in BookmarkStore/ClippingStore/
// SyncPending/RecentBooksStore/LibraryBuilder/Txt/Utf8/Xtc and more, none of which this guard
// depends on or this test needs to exercise.
#include "../../src/util/BookMoveUtils.h"

namespace BookMoveUtils {

bool isInArchiveFolder(const std::string& path) {
  constexpr size_t n = sizeof(ARCHIVE_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, ARCHIVE_FOLDER) == 0 && path[n] == '/';
}

}  // namespace BookMoveUtils
