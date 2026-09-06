#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct Ao3MarkedForLaterEntry {
  std::string path;
  std::string title;
  std::string author;

  bool operator==(const Ao3MarkedForLaterEntry& other) const { return path == other.path; }
};

// Dashboard Tab 0: fics the reader has explicitly flagged to read later.
// FIFO, capped at 10 -- oldest entry (least recently marked) sits at index 0
// and is the one evicted when a new mark pushes the list over the cap.
// Re-marking an already-present path is a true no-op (no promotion/reorder),
// unlike RecentBooksStore's always-promote-to-front semantics.
class Ao3MarkedForLaterStore : public PersistableStore<Ao3MarkedForLaterStore> {
 private:
  std::vector<Ao3MarkedForLaterEntry> entries;

  static constexpr int MAX_ENTRIES = 10;

  Ao3MarkedForLaterStore() = default;
  ~Ao3MarkedForLaterStore() = default;

  friend class PersistableStore<Ao3MarkedForLaterStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/marked_for_later.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // No-op (returns false) if already present or the list is at capacity.
  // Persists on success.
  bool addBook(const std::string& path, const std::string& title, const std::string& author);

  // Returns true if an entry was found and removed. Persists on success.
  bool removeByPath(const std::string& path);

  bool contains(const std::string& path) const;

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist -- caller decides.
  bool pruneMissing();

  const std::vector<Ao3MarkedForLaterEntry>& getEntries() const {
    ensureLoaded();
    return entries;
  }

  int getCount() const {
    ensureLoaded();
    return static_cast<int>(entries.size());
  }
};

#define AO3_MARKED_FOR_LATER_STORE Ao3MarkedForLaterStore::getInstance()
