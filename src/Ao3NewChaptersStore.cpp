#include "Ao3NewChaptersStore.h"

#include <HalStorage.h>

#include <algorithm>
#include <utility>

void Ao3NewChaptersStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (const auto& entry : entries) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = entry.path;
    obj["title"] = entry.title;
    obj["author"] = entry.author;
  }
}

bool Ao3NewChaptersStore::fromJson(JsonVariantConst doc) {
  entries.clear();
  JsonArrayConst arr = doc["entries"].as<JsonArrayConst>();
  entries.reserve(std::min(arr.size(), static_cast<size_t>(MAX_ENTRIES)));
  for (JsonObjectConst obj : arr) {
    if (entries.size() >= static_cast<size_t>(MAX_ENTRIES)) break;
    Ao3NewChaptersEntry entry;
    entry.path = obj["path"] | "";
    entry.title = obj["title"] | "";
    entry.author = obj["author"] | "";
    entries.push_back(entry);
  }
  return true;
}

void Ao3NewChaptersStore::addBook(const std::string& path, const std::string& title, const std::string& author) {
  ensureLoaded();

  auto it = std::find_if(entries.begin(), entries.end(), [&](const Ao3NewChaptersEntry& e) { return e.path == path; });
  if (it != entries.end()) {
    it->title = title;
    it->author = author;
    if (it != entries.begin()) {
      Ao3NewChaptersEntry entry = std::move(*it);
      entries.erase(it);
      entries.insert(entries.begin(), std::move(entry));
    }
    saveToFile();
    return;
  }
  if (entries.size() >= static_cast<size_t>(MAX_ENTRIES)) {
    return;
  }
  entries.insert(entries.begin(), {path, title, author});
  saveToFile();
}

bool Ao3NewChaptersStore::removeByPath(const std::string& path) {
  ensureLoaded();

  auto it = std::find_if(entries.begin(), entries.end(), [&](const Ao3NewChaptersEntry& e) { return e.path == path; });
  if (it == entries.end()) {
    return false;
  }
  entries.erase(it);
  saveToFile();
  return true;
}

bool Ao3NewChaptersStore::pruneMissing() {
  ensureLoaded();

  const size_t before = entries.size();
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const Ao3NewChaptersEntry& e) { return !Storage.exists(e.path.c_str()); }),
               entries.end());
  return entries.size() != before;
}
