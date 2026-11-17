#pragma once

#include <string>

#include "Print.h"

// Minimal stub: Ao3Librarian.cpp only calls getPath()/getCachePath()/getTitle()/getAuthor()/
// getSpineItemsCount()/getSpineItem()/hasAo3Info()/saveAo3Info()/readItemContentsToStream()/
// getWordCount()/getMainShip()/getOtherShips() (confirmed by grepping every `epub.` call site
// in that file). spineItemsCount defaults to 0, which makes Ao3Librarian::parseTitlePage()
// return false immediately without ever reaching readItemContentsToStream() -- deliberate, so
// this test exercises the real scrape()/parseTitlePage() control flow without needing to fake
// real EPUB zip/HTML content. The three OPF-sourced accessors default to empty, matching a file
// with no calibre:user_metadata block at all -- the baseline case the existing guard tests need
// unchanged; tests exercising the OPF-enrichment cascade set them explicitly.
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
  // Streams fakeTitlePageHtml when non-empty (lets a test make the real parseTitlePage()
  // succeed without faking a whole real title page -- just enough HTML to satisfy its own
  // success check, e.g. a bare work-ID link with no Words:/Relationship: label at all,
  // simulating the condensed FanFicFare template this cascade exists for).
  bool readItemContentsToStream(const std::string&, Print& out, size_t) const {
    if (fakeTitlePageHtml.empty()) return false;
    out.write(reinterpret_cast<const uint8_t*>(fakeTitlePageHtml.data()), fakeTitlePageHtml.size());
    return true;
  }

  bool hasAo3Info() const { return hasAo3InfoValue; }
  void saveAo3Info(const std::string&, const std::string&, bool) const { ++saveAo3InfoCalls; }

  const std::string& getWordCount() const { return wordCountValue; }
  const std::string& getMainShip() const { return mainShipValue; }
  const std::string& getOtherShips() const { return otherShipsValue; }

  int spineItemsCount = 0;
  bool hasAo3InfoValue = false;
  mutable int saveAo3InfoCalls = 0;
  std::string wordCountValue;
  std::string mainShipValue;
  std::string otherShipsValue;
  std::string fakeTitlePageHtml;

 private:
  std::string path_;
  std::string cachePath_;
  std::string title_ = "Test Fic";
  std::string author_ = "Test Author";
};
