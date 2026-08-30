#pragma once
#include <cstdint>
#include <string>

/**
 * The reading-position baseline recorded by the most recent sync (push or
 * pull), used to detect whether local progress has changed independently
 * since then -- see BookFusionBookIdStore::loadSyncBaseline().
 *
 * pageNumber/totalPages are -1/0 when unknown (a sidecar written before this
 * field existed, or one written by an "apply remote" that only had a
 * percentage/chapter to go on) -- callers fall back to a float-epsilon
 * compare against percentage/pagePositionInBook in that case.
 */
struct BookFusionSyncBaseline {
  bool hasBaseline = false;   // false if no sidecar, or the sidecar predates this field.
  std::string syncedAtUtcIso;  // Server-reported updated_at from the last successful sync, if any.
  float percentage = 0.0f;
  float pagePositionInBook = 0.0f;
  int chapterIndex = 0;
  int pageNumber = -1;
  int totalPages = 0;
};

/**
 * Per-book sidecar linking a local EPUB to its BookFusion book ID, plus the
 * reading-time-sync watermark and last-sync position baseline.
 *
 * Not a PersistableStore singleton -- one JSON file per book, living inside
 * that book's existing cache directory (Epub::cachePathForFilePath(), i.e.
 * /.crosspoint/epub_<fnvHash64(path)>/bookfusion.json), so it shares the same
 * path-hash identity the EPUB cache already uses. See BookCacheUtils.cpp,
 * where "bookfusion.json" is added to the cache-preservation lists so this
 * sidecar survives cache clears/rebuilds like progress.bin does.
 *
 * Every save is a read-modify-write against the existing sidecar doc, so
 * saving one field (e.g. the reading-time watermark) never clobbers another
 * (e.g. the book ID or sync baseline).
 */
class BookFusionBookIdStore {
 public:
  // Reads the sidecar for epubPath, or returns 0 if none exists.
  static uint32_t loadBookId(const std::string& epubPath);

  // Whether epubPath has a BookFusion sidecar at all -- convenience wrapper
  // for call sites that only care about linkage, not the actual ID (e.g. a
  // cover badge indicating "this book is BookFusion-linked").
  static bool hasBookId(const std::string& epubPath) { return loadBookId(epubPath) != 0; }

  // Writes (or overwrites) the sidecar for epubPath. Returns false on failure.
  static bool saveBookId(const std::string& epubPath, uint32_t bookId);

  // Removes the sidecar for epubPath, if any.
  static void clearBookId(const std::string& epubPath);

  // How many seconds of BookReadingStats::totalReadingSeconds have already
  // been pushed to BookFusion's reading-time-tracking endpoint. 0 if never
  // synced.
  static uint32_t loadSyncedReadingSeconds(const std::string& epubPath);
  static bool saveSyncedReadingSeconds(const std::string& epubPath, uint32_t seconds);

  // The position baseline recorded by the most recent sync.
  static BookFusionSyncBaseline loadSyncBaseline(const std::string& epubPath);
  static bool saveSyncBaseline(const std::string& epubPath, const BookFusionSyncBaseline& baseline);

 private:
  static std::string sidecarPath(const std::string& epubPath);
};
