#include "Ao3WipsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace {
std::string toLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// This store is deliberately unbounded (every author-incomplete fic in the
// library, not a curated recent-N list like its sibling dashboard stores),
// so a heavy user's history can grow it large over years of use. Guard the
// one place it actually grows -- a brand new entry, not an in-place update
// of an existing one -- rather than cap it and silently drop real WIPs.
constexpr uint32_t MIN_FREE_HEAP_FOR_NEW_ENTRY = 24576;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_NEW_ENTRY = 16384;

bool hasHeapForNewEntry() {
  return ESP.getFreeHeap() >= MIN_FREE_HEAP_FOR_NEW_ENTRY && ESP.getMaxAllocHeap() >= MIN_MAX_ALLOC_HEAP_FOR_NEW_ENTRY;
}
}  // namespace

void Ao3WipsStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = entry.path;
    obj["title"] = entry.title;
    obj["author"] = entry.author;
  }
}

bool Ao3WipsStore::fromJson(JsonVariantConst doc) {
  entries.clear();
  JsonArrayConst arr = doc["entries"].as<JsonArrayConst>();
  entries.reserve(arr.size());
  for (JsonObjectConst obj : arr) {
    Ao3WipEntry entry;
    entry.path = obj["path"] | "";
    entry.title = obj["title"] | "";
    entry.author = obj["author"] | "";
    entries.push_back(entry);
  }
  return true;
}

void Ao3WipsStore::addBook(const std::string& path, const std::string& title, const std::string& author) {
  ensureLoaded();

  auto it = std::find_if(entries.begin(), entries.end(), [&](const Ao3WipEntry& e) { return e.path == path; });
  if (it != entries.end()) {
    if (it->title == title && it->author == author) {
      return;
    }
    entries.erase(it);
  } else if (!hasHeapForNewEntry()) {
    // A genuinely new WIP on a low-heap device: skip rather than risk an
    // OOM abort growing this unbounded vector. An in-place update above
    // (erase+reinsert, same net size) isn't gated -- only real growth is.
    LOG_ERR("Ao3Wips", "Skipping new WIP entry, low heap: %u free, %u max alloc", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return;
  }
  const std::string lowerTitle = toLowerAscii(title);
  const auto insertIt = std::lower_bound(entries.begin(), entries.end(), lowerTitle,
                                         [](const Ao3WipEntry& e, const std::string& key) {
                                           return toLowerAscii(e.title) < key;
                                         });
  entries.insert(insertIt, {path, title, author});
  saveToFile();
}

bool Ao3WipsStore::removeByPath(const std::string& path) {
  ensureLoaded();

  auto it = std::find_if(entries.begin(), entries.end(), [&](const Ao3WipEntry& e) { return e.path == path; });
  if (it == entries.end()) {
    return false;
  }
  entries.erase(it);
  saveToFile();
  return true;
}

bool Ao3WipsStore::pruneMissing() {
  ensureLoaded();

  const size_t before = entries.size();
  entries.erase(
      std::remove_if(entries.begin(), entries.end(), [](const Ao3WipEntry& e) { return !Storage.exists(e.path.c_str()); }),
      entries.end());
  return entries.size() != before;
}
