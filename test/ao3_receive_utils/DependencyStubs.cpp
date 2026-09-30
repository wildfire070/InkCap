// Link-time stubs for the handful of production functions renameToTitleAuthor()/
// replaceExisting() call into. Those two functions pull in real Ao3Librarian/BookMoveUtils/
// BookCacheUtils production logic (the AO3 index database, book-move state migration, cache
// management) that isn't worth stubbing further just to unit-test Ao3ReceiveUtils's other,
// pure functions -- so this test target intentionally does not exercise renameToTitleAuthor()
// or replaceExisting() in depth. The real headers are used as-is (they're lightweight,
// declaration-only), just their .cpp implementations are swapped out here.
#include "../../src/Ao3Librarian.h"
#include "../../src/util/BookCacheUtils.h"
#include "../../src/util/BookMoveUtils.h"

bool Ao3Librarian::tombstoneRecord(const std::string&) { return true; }

bool BookMoveUtils::migrateMovedEpubState(const std::string&, const std::string&, const std::string&,
                                          const std::string&, const std::string&, bool) {
  return true;
}

bool clearBookCachePreservingUserState(const std::string&) { return true; }
