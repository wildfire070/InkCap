#pragma once
#include <Arena.h>
#include <Print.h>

#include <algorithm>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_DC_SUBJECT,
    IN_DC_IDENTIFIER,
    IN_DC_SOURCE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  const bool metadataOnly;
  bool metadataComplete = false;
  HalFile tempItemStore;
  std::string coverItemId;
  Arena itemIndexArena;
  bool parseFailed = false;
  bool lowMemoryFailure = false;
  // XML character data can arrive in several write() calls for one text node
  // (notably around entity references). Tracked as element state rather than
  // inferred per-callback, so a title or author split across callbacks still
  // collapses whitespace and separators correctly.
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;
  bool titleTruncated = false;
  bool authorTruncated = false;
  bool languageTruncated = false;
  bool hasExplicitStartReference = false;
  bool collectCssFiles = true;

  // Index for compact idref->href lookup. The temp manifest rows retain the
  // full ID for collision-safe matching without retaining IDs in heap memory.
  struct ItemIndexEntry {
    uint64_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  static constexpr size_t ITEM_INDEX_CHUNK_CAPACITY = 240;
  struct ItemIndexChunk {
    ItemIndexChunk* next = nullptr;
    uint16_t count = 0;
    ItemIndexEntry entries[ITEM_INDEX_CHUNK_CAPACITY];
  };
  ItemIndexChunk* itemIndexHead = nullptr;
  ItemIndexChunk* itemIndexTail = nullptr;
  size_t itemIndexCount = 0;
  size_t itemIndexChunkCount = 0;

  bool appendItemIndexEntry(const ItemIndexEntry& entry);
  void sortItemIndexChunks();
  bool findItemHref(const std::string& idref, std::string& href);
  static bool itemIndexEntryLess(const ItemIndexEntry& lhs, const ItemIndexEntry& rhs) {
    return lhs.idHash < rhs.idHash || (lhs.idHash == rhs.idHash && lhs.idLen < rhs.idLen);
  }

  // FNV-1a hash function
  static uint64_t fnvHash(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }
  static uint64_t fnvHash(const std::string& s) { return fnvHash(s.c_str(), s.size()); }
  static uint64_t fnvHash(const char* s) {
    if (!s) return 0;
    uint64_t hash = 14695981039346656037ull;
    while (*s != '\0') {
      hash ^= static_cast<uint8_t>(*s);
      hash *= 1099511628211ull;
      ++s;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  // Bounds title/author/language/subjectBuffer against a runaway or
  // malformed field in an untrusted EPUB's content.opf -- matches the cap
  // Epub.cpp's DescriptionParser already applies to dc:description, for the
  // same reason (real book metadata is far smaller than this).
  static constexpr size_t kMaxFieldBytes = 4096;

  std::string title;
  std::string author;

  // Book IDs, recorded into book-ids.json by Epub::parseContentOpf so any feature can match a book by
  // identity rather than filename: AO3 work ID from a dc:source/dc:identifier work URL (FanFicFare and
  // Calibre exports), BookFusion book ID from <dc:identifier opf:scheme="BOOKFUSION"> (Calibre's plugin).
  std::string ao3WorkId;
  uint32_t bookFusionId = 0;
  std::string identifierBuffer;
  bool identifierIsBookFusion = false;  // the dc:identifier currently being read carries that scheme
  std::string language;
  std::string tocNcxPath;
  std::string tocNavPath;        // EPUB 3 nav document path
  std::string guideTocPageHref;  // EPUB 2 guide TOC page, if declared
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  // dc:subject tags (Calibre convention), e.g. genre/keyword tags a Calibre
  // library or BookFusion export attaches. A book can have several dc:subject
  // elements; subjectBuffer accumulates the current one's character data,
  // joined into tags (", "-separated) on each closing tag.
  std::string tags;
  std::string subjectBuffer;

  // BookFusion's "bookshelf" Calibre custom column (calibre:user_metadata:#bookfusionshelf),
  // written into the EPUB's own OPF metadata at export time -- no network call needed.
  std::string bookshelf;

  std::string seriesName;   // calibre:series
  std::string seriesIndex;  // calibre:series_index

  // User's own Calibre custom column for content rating (Explicit/Mature/General/
  // Teen/-), distinct from Calibre's built-in numeric star-rating field and from
  // Ao3Librarian's own AO3-preface-scraped rating (same values, separate pipeline).
  std::string contentRating;  // calibre:user_metadata:#rating (#value# extracted)

  std::string chapters;          // calibre:user_metadata:#chapters, e.g. "1/1"
  std::string completionStatus;  // calibre:user_metadata:#completionstatus, e.g. "Completed"
  // Story's own last-update date on AO3 (calibre:user_metadata:#updated), truncated to
  // YYYY-MM-DD.
  std::string updatedDate;
  bool liked = false;       // calibre:user_metadata:#like
  bool readStatus = false;  // calibre:user_metadata:#readstatus

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache, const bool collectCssFiles = true,
                            const bool metadataOnly = false)
      : cachePath(cachePath),
        baseContentPath(baseContentPath),
        remainingSize(xmlSize),
        cache(cache),
        metadataOnly(metadataOnly),
        collectCssFiles(collectCssFiles) {}
  ~ContentOpfParser() override;

  bool setup();
  bool failedForLowMemory() const { return lowMemoryFailure; }

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
