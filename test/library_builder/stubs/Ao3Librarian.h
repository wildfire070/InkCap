#pragma once

#include <string>

// Narrow stub: BookCacheUtils.cpp only calls the one static method below (to
// tombstone an AO3 index record on cache-clear when the book has AO3 info,
// which none of this test's fixtures do), not the real scraper/index class --
// pulling in the real header would drag in Ao3CompactIndexRecord.h and the
// rest of the AO3 index machinery this test has no need to exercise.
//
// NOTE: LibraryBuilder.cpp's own `#include "../../src/Ao3Librarian.h"` is a relative
// path, so it always sees the REAL header regardless of this stub -- this file only
// shadows the unqualified `#include "Ao3Librarian.h"` BookCacheUtils.cpp uses. The
// definitions LibraryBuilder.cpp's new calls need (writeIndexRecord/mapRating) live in
// Ao3LibrarianDefs.cpp instead, which matches the real header's declarations exactly.
class Ao3Librarian {
 public:
  static bool tombstoneRecord(const std::string&) { return false; }
};
