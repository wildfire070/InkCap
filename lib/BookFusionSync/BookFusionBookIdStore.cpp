#include "BookFusionBookIdStore.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

namespace {
constexpr char SIDECAR_FILENAME[] = "bookfusion.json";

// Loads the existing sidecar doc, if any. Returns false (leaving doc empty)
// if there is no sidecar yet or it couldn't be read -- callers proceed with
// an empty doc so a save still writes the field(s) they're setting.
bool loadSidecarDoc(const std::string& path, JsonDocument& doc) {
  if (!Storage.exists(path.c_str())) return false;
  return PersistableStoreBase::readDocFromFile(path.c_str(), doc);
}

bool writeSidecarDoc(const std::string& epubPath, const std::string& path, JsonDocument& doc) {
  const std::string cacheDir = Epub::cachePathForFilePath(epubPath, "/.crosspoint");
  Storage.mkdir(cacheDir.c_str());
  return PersistableStoreBase::writeDocToFile(path.c_str(), doc);
}
}  // namespace

std::string BookFusionBookIdStore::sidecarPath(const std::string& epubPath) {
  return Epub::cachePathForFilePath(epubPath, "/.crosspoint") + "/" + SIDECAR_FILENAME;
}

uint32_t BookFusionBookIdStore::loadBookId(const std::string& epubPath) {
  JsonDocument doc;
  if (!loadSidecarDoc(sidecarPath(epubPath), doc)) return 0;
  return doc["bookId"] | (uint32_t)0;
}

bool BookFusionBookIdStore::saveBookId(const std::string& epubPath, uint32_t bookId) {
  if (bookId == 0) {
    LOG_ERR("BFS", "Refusing to save sidecar with bookId=0 (reserved for 'no id')");
    return false;
  }

  const std::string path = sidecarPath(epubPath);
  JsonDocument doc;
  loadSidecarDoc(path, doc);
  doc["bookId"] = bookId;
  return writeSidecarDoc(epubPath, path, doc);
}

void BookFusionBookIdStore::clearBookId(const std::string& epubPath) {
  const std::string path = sidecarPath(epubPath);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}

uint32_t BookFusionBookIdStore::loadSyncedReadingSeconds(const std::string& epubPath) {
  JsonDocument doc;
  if (!loadSidecarDoc(sidecarPath(epubPath), doc)) return 0;
  return doc["syncedReadingSeconds"] | (uint32_t)0;
}

bool BookFusionBookIdStore::saveSyncedReadingSeconds(const std::string& epubPath, uint32_t seconds) {
  const std::string path = sidecarPath(epubPath);
  JsonDocument doc;
  loadSidecarDoc(path, doc);
  doc["syncedReadingSeconds"] = seconds;
  return writeSidecarDoc(epubPath, path, doc);
}

BookFusionSyncBaseline BookFusionBookIdStore::loadSyncBaseline(const std::string& epubPath) {
  BookFusionSyncBaseline baseline;
  JsonDocument doc;
  if (!loadSidecarDoc(sidecarPath(epubPath), doc)) return baseline;
  if (!doc["lastSyncAt"].is<const char*>()) return baseline;  // sidecar predates this field

  baseline.hasBaseline = true;
  baseline.syncedAtUtcIso = doc["lastSyncAt"] | "";
  baseline.percentage = doc["lastSyncPercentage"] | 0.0f;
  baseline.pagePositionInBook = doc["lastSyncPagePositionInBook"] | 0.0f;
  baseline.chapterIndex = doc["lastSyncChapterIndex"] | 0;
  baseline.pageNumber = doc["lastSyncPageNumber"] | -1;
  baseline.totalPages = doc["lastSyncTotalPages"] | 0;
  return baseline;
}

bool BookFusionBookIdStore::saveSyncBaseline(const std::string& epubPath, const BookFusionSyncBaseline& baseline) {
  const std::string path = sidecarPath(epubPath);
  JsonDocument doc;
  loadSidecarDoc(path, doc);
  doc["lastSyncAt"] = baseline.syncedAtUtcIso;
  doc["lastSyncPercentage"] = baseline.percentage;
  doc["lastSyncPagePositionInBook"] = baseline.pagePositionInBook;
  doc["lastSyncChapterIndex"] = baseline.chapterIndex;
  doc["lastSyncPageNumber"] = baseline.pageNumber;
  doc["lastSyncTotalPages"] = baseline.totalPages;
  return writeSidecarDoc(epubPath, path, doc);
}
