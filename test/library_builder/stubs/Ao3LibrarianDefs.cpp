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
#include "HalStorage.h"

namespace Ao3LibrarianDefs {
std::vector<CompactIndexRecord> writtenRecords;
}  // namespace Ao3LibrarianDefs

bool Ao3Librarian::writeIndexRecord(const CompactIndexRecord& rec) {
  Ao3LibrarianDefs::writtenRecords.push_back(rec);
  return true;
}

// Real file read against the fake filesystem (unlike writeIndexRecord() above, which only
// captures writes into writtenRecords and never actually persists anything) -- a test that
// pre-seeds /.crosspoint/ao3_library_index.bin via fake::add() needs this to see it. No
// IndexWriteBatch-cache optimization: that's a performance detail of the real
// Ao3Librarian.cpp, already covered by its own dedicated test target, not something
// LibraryBuilderTest needs to re-verify -- a plain scan is correct, just not fast.
bool Ao3Librarian::findLiveIndexRecord(const uint64_t cacheHash, CompactIndexRecord& out) {
  HalFile f;
  if (!Storage.openFileForRead("AO3L", "/.crosspoint/ao3_library_index.bin", f)) return false;

  char magic[4];
  uint8_t version;
  uint16_t recordCount;
  const bool headerOk = f.read(magic, 4) == 4 && f.read(&version, 1) == 1 && f.read((uint8_t*)&recordCount, 2) == 2;
  if (!headerOk || memcmp(magic, "AO3X", 4) != 0 || version != 4) {
    f.close();
    return false;
  }

  f.seek(12);  // past nextSequence + reserved, the rest of the 12-byte header
  CompactIndexRecord rec;
  for (uint16_t i = 0; i < recordCount; i++) {
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    if (!(rec.flags & 1) && rec.cacheHash == cacheHash) {
      out = rec;
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

// No-op: writeIndexRecord()'s stub above doesn't implement real index-file batching, so
// there's no cache state to manage here -- just needs to link against LibraryBuilder.cpp's
// real-header-matching Ao3Librarian::IndexWriteBatch usage.
Ao3Librarian::IndexWriteBatch::IndexWriteBatch() {}
Ao3Librarian::IndexWriteBatch::~IndexWriteBatch() {}

char Ao3Librarian::mapRating(const char* s) {
  if (!s) return '-';
  if (strcasestr(s, "General")) return 'G';
  if (strcasestr(s, "Teen")) return 'T';
  if (strcasestr(s, "Mature")) return 'M';
  if (strcasestr(s, "Explicit")) return 'E';
  return '-';
}
