#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct Ao3WipEntry {
  std::string path;
  std::string title;
  std::string author;

  bool operator==(const Ao3WipEntry& other) const { return path == other.path; }
};

// Dashboard Tab 2: fics the author hasn't finished posting yet (author-side
// incompleteness, i.e. Ao3LibraryMetadata::isCompleted == false -- distinct
// from the reader's own BookStatus::FINISHED). Unbounded, kept sorted
// alphabetically by title. addBook() on an already-present path updates its
// metadata in place rather than inserting a duplicate.
class Ao3WipsStore : public PersistableStore<Ao3WipsStore> {
 private:
  std::vector<Ao3WipEntry> entries;

  Ao3WipsStore() = default;
  ~Ao3WipsStore() = default;

  friend class PersistableStore<Ao3WipsStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/wips.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Inserts in sorted position, or updates metadata in place if already
  // present. Persists on success.
  void addBook(const std::string& path, const std::string& title, const std::string& author);

  // Returns true if an entry was found and removed. Persists on success.
  bool removeByPath(const std::string& path);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist -- caller decides.
  bool pruneMissing();

  const std::vector<Ao3WipEntry>& getEntries() const {
    ensureLoaded();
    return entries;
  }

  int getCount() const {
    ensureLoaded();
    return static_cast<int>(entries.size());
  }
};

#define AO3_WIPS_STORE Ao3WipsStore::getInstance()
