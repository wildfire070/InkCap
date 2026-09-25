#include "BookIds.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "../Epub.h"

namespace {
constexpr char CACHE_ROOT[] = "/.crosspoint";
constexpr char CACHE_DIR_PREFIX[] = "epub_";

std::string idsFilePath(const std::string& cachePath) { return cachePath + "/" + BookIds::FILE_NAME; }
}  // namespace

namespace BookIds {

bool exists(const std::string& cachePath) { return Storage.exists(idsFilePath(cachePath).c_str()); }

bool load(const std::string& cachePath, Ids& out) {
  out = Ids{};
  const std::string file = idsFilePath(cachePath);
  if (!Storage.exists(file.c_str())) return false;
  const String json = Storage.readFile(file.c_str());
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  out.ao3WorkId = doc["ao3"] | "";
  out.bookFusionId = doc["bf"] | 0u;
  out.path = doc["path"] | "";
  return true;
}

void record(const std::string& epubPath, const std::string& ao3WorkId, const uint32_t bookFusionId) {
  const std::string cachePath = Epub::cachePathForFilePath(epubPath, CACHE_ROOT);
  if (!Storage.exists(cachePath.c_str())) return;

  Ids ids;
  const bool hadFile = load(cachePath, ids);
  const Ids before = ids;
  if (!ao3WorkId.empty()) ids.ao3WorkId = ao3WorkId;
  if (bookFusionId != 0) ids.bookFusionId = bookFusionId;
  ids.path = epubPath;

  if (hadFile && ids.ao3WorkId == before.ao3WorkId && ids.bookFusionId == before.bookFusionId &&
      ids.path == before.path) {
    return;
  }

  JsonDocument doc;
  doc["ao3"] = ids.ao3WorkId;
  doc["bf"] = ids.bookFusionId;
  doc["path"] = ids.path;
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(idsFilePath(cachePath).c_str(), json)) {
    LOG_ERR("BIDS", "Could not write %s", idsFilePath(cachePath).c_str());
  }
}

bool findOtherCopy(const std::string& epubPath, const Ids& wanted, std::string& outPath) {
  outPath.clear();
  if (wanted.empty()) return false;

  const std::string ownCache = Epub::cachePathForFilePath(epubPath, CACHE_ROOT);
  auto root = Storage.open(CACHE_ROOT);
  if (!root || !root.isDirectory()) return false;

  char name[64];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!isDir || strncmp(name, CACHE_DIR_PREFIX, sizeof(CACHE_DIR_PREFIX) - 1) != 0) continue;

    const std::string candidateCache = std::string(CACHE_ROOT) + "/" + name;
    if (candidateCache == ownCache) continue;

    Ids ids;
    if (!load(candidateCache, ids) || ids.path.empty() || ids.path == epubPath) continue;
    const bool sameAo3 = !wanted.ao3WorkId.empty() && ids.ao3WorkId == wanted.ao3WorkId;
    const bool sameBookFusion = wanted.bookFusionId != 0 && ids.bookFusionId == wanted.bookFusionId;
    if (!sameAo3 && !sameBookFusion) continue;

    // A moved or deleted book leaves a stale path behind: it must still be there, and its own cache dir.
    if (!Storage.exists(ids.path.c_str())) continue;
    if (Epub::cachePathForFilePath(ids.path, CACHE_ROOT) != candidateCache) continue;

    outPath = ids.path;
    root.close();
    return true;
  }
  root.close();
  return false;
}

}  // namespace BookIds
