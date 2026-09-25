#pragma once
#include <Memory.h>
#include <stdint.h>

#pragma pack(push, 1)
struct CompactIndexRecord {
  char title[64];          // truncated title
  char author[32];         // truncated author
  uint32_t wordCount;      // from scraping
  uint32_t addedSequence;  // monotonically increasing counter assigned at index write time
  char seriesName[32];     // from scraping
  uint16_t seriesPart;     // position within series, 0 if none
  char fandom[32];         // scraped from epub HTML
  char relationship1[32];  // primary pairing — scraped from epub HTML
  char relationship2[32];  // secondary pairing — scraped from epub HTML
  uint64_t cacheHash;      // ZipFile::fnvHash64(epubPath) — matches the epub cache dir name
  char rating;             // G, T, M, E, - (same encoding as Ao3LibraryMetadata::rating)
  uint8_t isCompleted;     // 0 or 1, from Ao3LibraryMetadata::isCompleted
  uint8_t flags;           // bit 0 = tombstone (deleted), bit 1 = finished (user-marked)
};
#pragma pack(pop)

// Exactly 245 bytes on disk
static_assert(sizeof(CompactIndexRecord) == 245, "CompactIndexRecord must be exactly 245 bytes");

// How many live books the AO3 library will hold depends on the device: each takes ~44 bytes of RAM
// while the library is open (see ViewEntry), plus hash sets and sort copies. Devices without PSRAM
// (ESP32-C3: X3/X4) keep 1000; PSRAM devices (ESP32-S3: X4 Pro, Sticky) allow 2000. Adding a book past
// the cap fails, and an oversize index is shown truncated rather than rejected.
constexpr uint16_t MAX_LIBRARY_BOOKS_LOW_RAM = 1000;
constexpr uint16_t MAX_LIBRARY_BOOKS_PSRAM = 2000;
inline uint16_t maxLibraryBooks() {
  return psramHeapAvailable() ? MAX_LIBRARY_BOOKS_PSRAM : MAX_LIBRARY_BOOKS_LOW_RAM;
}

// File-format sanity limit, NOT a device limit: a header claiming more records than this is corrupt.
// Deliberately above every device's cap so a card moved between an S3 and a C3 device never has its
// index treated as corrupt (and deleted) just because the other device stored more books.
constexpr uint16_t MAX_INDEX_RECORDS = 5000;
constexpr uint32_t INDEX_HEADER_SIZE = 12;

inline uint32_t offsetOf(uint16_t i) { return INDEX_HEADER_SIZE + i * (uint32_t)sizeof(CompactIndexRecord); }
