#pragma once

#include <string>

// Minimal stub: only renameToTitleAuthor()/replaceExisting() touch this class, and neither
// is exercised in depth by this test target (they pull in real Ao3Librarian/BookMoveUtils/
// BookCacheUtils production logic that isn't worth stubbing further just to unit-test
// Ao3ReceiveUtils's other, pure functions). This exists only so the file compiles.
class Epub {
 public:
  Epub(const std::string& path, const char*) : path_(path) {}

  static std::string cachePathForFilePath(const std::string&, const std::string& cacheDir) {
    return cacheDir + "/cache";
  }

  bool clearCache() const { return true; }
  void setupCacheDir() const {}

 private:
  std::string path_;
};
