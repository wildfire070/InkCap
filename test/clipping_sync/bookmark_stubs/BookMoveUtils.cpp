// Minimal stub for the one BookMoveUtils function BookmarkStore.cpp calls
// (isInArchiveFolder) -- real logic, no SD-card dependency, so no behavioral
// drift from production. This test target does not link BookMoveUtils.cpp
// itself (upstream's BookmarkStore.cpp has no AO3/archive awareness), and
// isInArchiveFolder is an InkCap-side addition the new upstream test harness
// doesn't know to stub.
#include "util/BookMoveUtils.h"

namespace BookMoveUtils {

bool isInArchiveFolder(const std::string& path) {
  constexpr size_t n = sizeof(ARCHIVE_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, ARCHIVE_FOLDER) == 0 && path[n] == '/';
}

}  // namespace BookMoveUtils
