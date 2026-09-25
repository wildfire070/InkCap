#pragma once

#include <cstdint>
#include <string>

// Per-book identifiers that say "this is the same story/book" regardless of filename: the AO3
// work ID and the BookFusion book ID. Recorded whenever a book's metadata is read (opening it,
// building its cache, indexing it, downloading it) into book-ids.json inside its cache dir,
// which a cache clear preserves. Every firmware branch reads and writes the same file, so IDs
// captured on one branch are there for any other feature or branch that can use them.
namespace BookIds {

constexpr char FILE_NAME[] = "book-ids.json";

struct Ids {
  std::string ao3WorkId;      // digits only; empty if unknown
  uint32_t bookFusionId = 0;  // 0 if unknown
  std::string path;           // where the book was when this was written; verify before trusting

  bool empty() const { return ao3WorkId.empty() && bookFusionId == 0; }
};

bool exists(const std::string& cachePath);
bool load(const std::string& cachePath, Ids& out);

// Merges IDs into epubPath's sidecar: a non-empty ID replaces the stored one, an empty one never
// clears it, and the current path is always refreshed. Never creates the cache dir (a Library scan
// must not litter the SD card with one per book); does nothing if it doesn't exist. Writes the
// file even when both IDs are empty, so backfill logic can tell "checked, none" from "never checked".
void record(const std::string& epubPath, const std::string& ao3WorkId, uint32_t bookFusionId);

// Copies IDs already sitting in older per-book sidecars into book-ids.json: the AO3 work ID from
// ao3-info.bin (the only place a native AO3 download's ID exists) and the BookFusion ID from
// bookfusion.json. Lets books indexed or downloaded before book-ids.json existed be picked up
// without re-indexing or re-downloading. No-op if the cache dir doesn't exist.
void importLegacySidecars(const std::string& epubPath);

// Finds another book that has one of wanted's IDs. Only books with a sidecar are considered, and a
// match must still exist at the path its sidecar names (whose cache dir must be its own).
bool findOtherCopy(const std::string& epubPath, const Ids& wanted, std::string& outPath);

// First run of digits in text, or 0 ("bookfusion:4883231" and "4883231" both give 4883231).
// Inline so the OPF parser (and its host test) need nothing else from this module.
inline uint32_t parseBookFusionId(const std::string& text) {
  uint64_t value = 0;
  bool inDigits = false;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      inDigits = true;
      value = value * 10 + static_cast<uint64_t>(c - '0');
      if (value > UINT32_MAX) return 0;
    } else if (inDigits) {
      break;
    }
  }
  return static_cast<uint32_t>(value);
}

}  // namespace BookIds
