#include "Ao3WipsStore.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>

namespace {
std::string toLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
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
