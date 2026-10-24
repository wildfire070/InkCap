// BookmarkStore.cpp's own quoted #include "util/BookMoveUtils.h" resolves relative to its own
// directory, so a stub header on the include path can't shadow it -- this instead provides just
// the one symbol BookmarkStore.cpp links against, matching the real header's declared (non-inline)
// signature. The real BookMoveUtils.cpp pulls in Epub, LibraryBuilder, Txt, Xtc, RecentBooksStore
// and sync files -- all far outside what this persistence test exercises.
#include "util/BookMoveUtils.h"

namespace BookMoveUtils {

// Always false here: no test in this suite puts a book under ARCHIVE_FOLDER, so the real
// function's archive-recovery scan (BookmarkStore::loadForBook's Storage.listFiles() branch)
// never needs to run.
bool isInArchiveFolder(const std::string&) { return false; }

}  // namespace BookMoveUtils
