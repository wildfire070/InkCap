#pragma once

#include <string>

#include "Print.h"

// Minimal stub: Ao3Librarian.cpp only calls getPath()/getCachePath()/getTitle()/getAuthor()/
// getSpineItemsCount()/getSpineItem()/hasAo3Info()/saveAo3Info()/readItemContentsToStream()
// (confirmed by grepping every `epub.` call site in that file). spineItemsCount defaults to 0,
// which makes Ao3Librarian::parseTitlePage() return false immediately without ever reaching
// readItemContentsToStream() -- deliberate, so this test exercises the real scrape()/
// parseTitlePage() control flow without needing to fake real EPUB zip/HTML content.
class Epub {
 public:
  Epub(const std::string& path, const std::string& cacheRoot)
      : path_(path), cachePath_(cacheRoot + "/epub_" + std::to_string(std::hash<std::string>{}(path))) {}

  const std::string& getPath() const { return path_; }
  const std::string& getCachePath() const { return cachePath_; }
  const std::string& getTitle() const { return title_; }
  const std::string& getAuthor() const { return author_; }

  int getSpineItemsCount() const { return spineItemsCount; }
  struct SpineItem {
    std::string href;
  };
  SpineItem getSpineItem(int) const { return {}; }
  bool readItemContentsToStream(const std::string&, Print&, size_t) const { return false; }

  bool hasAo3Info() const { return hasAo3InfoValue; }
  void saveAo3Info(const std::string&, const std::string&, bool) const { ++saveAo3InfoCalls; }

  int spineItemsCount = 0;
  bool hasAo3InfoValue = false;
  mutable int saveAo3InfoCalls = 0;

 private:
  std::string path_;
  std::string cachePath_;
  std::string title_ = "Test Fic";
  std::string author_ = "Test Author";
};
