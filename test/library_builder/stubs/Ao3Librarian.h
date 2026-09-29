#pragma once

#include <string>

// Narrow stub: BookCacheUtils.cpp only calls the one static method below (to
// tombstone an AO3 index record on cache-clear when the book has AO3 info,
// which none of this test's fixtures do), not the real scraper/index class --
// pulling in the real header would drag in Ao3CompactIndexRecord.h and the
// rest of the AO3 index machinery this test has no need to exercise.
class Ao3Librarian {
 public:
  static bool tombstoneRecord(const std::string&) { return false; }
};
