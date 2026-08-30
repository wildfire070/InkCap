#include "LibraryScan.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

namespace LibraryScan {

namespace {
void scanDirectory(const std::string& dirPath, const BookVisitor& visitor, size_t& count) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.') {
      // Skip hidden directories/files: /.crosspoint (cache/sidecars),
      // /.sleep (sleep-screen assets), and any other dotfile.
      file.close();
      continue;
    }

    std::string childPath = dirPath;
    if (childPath.empty() || childPath.back() != '/') childPath += '/';
    childPath += name;

    if (file.isDirectory()) {
      file.close();  // Close before recursing -- one open handle per level, not one per book.
      scanDirectory(childPath, visitor, count);
      continue;
    }
    if (FsHelpers::hasEpubExtension(childPath) || FsHelpers::hasXtcExtension(childPath)) {
      count++;
      visitor(childPath);
    }
    file.close();
  }
  dir.close();
}
}  // namespace

void enumerateBooks(const BookVisitor& visitor) {
  size_t count = 0;
  scanDirectory("/", visitor, count);
  LOG_DBG("LibScan", "enumerateBooks: %zu books found", count);
}

}  // namespace LibraryScan
