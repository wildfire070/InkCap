#include "Ao3MarkedForLaterStore.h"

#include <HalStorage.h>

#include <algorithm>

void Ao3MarkedForLaterStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = entry.path;
    obj["title"] = entry.title;
    obj["author"] = entry.author;
  }
}

bool Ao3MarkedForLaterStore::fromJson(JsonVariantConst doc) {
  entries.clear();
  JsonArrayConst arr = doc["entries"].as<JsonArrayConst>();
  entries.reserve(std::min(arr.size(), static_cast<size_t>(MAX_ENTRIES)));
  for (JsonObjectConst obj : arr) {
    if (entries.size() >= static_cast<size_t>(MAX_ENTRIES)) break;
    Ao3MarkedForLaterEntry entry;
    entry.path = obj["path"] | "";
    entry.title = obj["title"] | "";
    entry.author = obj["author"] | "";
    entries.push_back(entry);
  }
  return true;
}

bool Ao3MarkedForLaterStore::addBook(const std::string& path, const std::string& title, const std::string& author) {
  ensureLoaded();

  if (contains(path)) {
    return false;
  }
  if (entries.size() >= static_cast<size_t>(MAX_ENTRIES)) {
    return false;
  }
  entries.push_back({path, title, author});
  saveToFile();
  return true;
}

bool Ao3MarkedForLaterStore::removeByPath(const std::string& path) {
  ensureLoaded();

  auto it = std::find_if(entries.begin(), entries.end(), [&](const Ao3MarkedForLaterEntry& e) { return e.path == path; });
  if (it == entries.end()) {
    return false;
  }
  entries.erase(it);
  saveToFile();
  return true;
}

bool Ao3MarkedForLaterStore::contains(const std::string& path) const {
  ensureLoaded();
  return std::find_if(entries.begin(), entries.end(), [&](const Ao3MarkedForLaterEntry& e) { return e.path == path; }) !=
         entries.end();
}

bool Ao3MarkedForLaterStore::pruneMissing() {
  ensureLoaded();

  const size_t before = entries.size();
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const Ao3MarkedForLaterEntry& e) { return !Storage.exists(e.path.c_str()); }),
               entries.end());
  return entries.size() != before;
}
