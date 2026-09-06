#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct Ao3NewChaptersEntry {
  std::string path;
  std::string title;
  std::string author;

  bool operator==(const Ao3NewChaptersEntry& other) const { return path == other.path; }
};

// Dashboard Tab 1: fics with a newly detected chapter, most-recently-updated
// first. Capped at 10, MRU -- addBook() promotes an existing entry to the
// front on re-add, and a new book is simply not added once the list is full
// (no auto-eviction of a still-fresh entry to make room).
class Ao3NewChaptersStore : public PersistableStore<Ao3NewChaptersStore> {
 private:
  std::vector<Ao3NewChaptersEntry> entries;

  static constexpr int MAX_ENTRIES = 10;

  Ao3NewChaptersStore() = default;
  ~Ao3NewChaptersStore() = default;

  friend class PersistableStore<Ao3NewChaptersStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/new_chapters.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Insert at front, or promote an existing entry to front and refresh its
  // metadata. No-op if not already present and the list is at capacity.
  // Persists on success (always, since even a promote-only change reorders).
  void addBook(const std::string& path, const std::string& title, const std::string& author);

  // Returns true if an entry was found and removed. Persists on success.
  bool removeByPath(const std::string& path);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist -- caller decides.
  bool pruneMissing();

  const std::vector<Ao3NewChaptersEntry>& getEntries() const {
    ensureLoaded();
    return entries;
  }

  int getCount() const {
    ensureLoaded();
    return static_cast<int>(entries.size());
  }
};

#define AO3_NEW_CHAPTERS_STORE Ao3NewChaptersStore::getInstance()
