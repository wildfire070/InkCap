// Minimal definitions for the two Ao3Librarian statics LibraryBuilder.cpp's AO3
// piggyback-write logic calls. LibraryBuilder.cpp includes the REAL Ao3Librarian.h via a
// relative path ("../../src/Ao3Librarian.h"), bypassing the narrow local stub entirely (that
// stub only shadows BookCacheUtils.cpp's unqualified include) -- so the linker needs real
// definitions matching the real header's declarations, not another header-level stub. The
// real Ao3Librarian.cpp itself isn't linked into this test target: its own HTML-scrape/
// network/storage dependencies have no place here, and its real behavior is covered by its
// own dedicated test target (Ao3LibrarianScrapeGuardTest). writtenRecords makes the record
// this logic would have written observable to LibraryBuilderTest's own tests.
#include <cstring>
#include <vector>

#include "../../../src/Ao3CompactIndexRecord.h"
#include "../../../src/Ao3Librarian.h"

namespace Ao3LibrarianDefs {
std::vector<CompactIndexRecord> writtenRecords;
}  // namespace Ao3LibrarianDefs

bool Ao3Librarian::writeIndexRecord(const CompactIndexRecord& rec) {
  Ao3LibrarianDefs::writtenRecords.push_back(rec);
  return true;
}

char Ao3Librarian::mapRating(const char* s) {
  if (!s) return '-';
  if (strcasestr(s, "General")) return 'G';
  if (strcasestr(s, "Teen")) return 'T';
  if (strcasestr(s, "Mature")) return 'M';
  if (strcasestr(s, "Explicit")) return 'E';
  return '-';
}
