#pragma once
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
  uint32_t cacheHash;      // uint32_t cast of std::hash<std::string>{}(epubPath)
  char rating;             // G, T, M, E, - (same encoding as Ao3LibraryMetadata::rating)
  uint8_t isCompleted;     // 0 or 1, from Ao3LibraryMetadata::isCompleted
  uint8_t flags;           // bit 0 = tombstone (deleted), bit 1 = finished (user-marked)
};
#pragma pack(pop)

// Exactly 241 bytes on disk
static_assert(sizeof(CompactIndexRecord) == 241, "CompactIndexRecord must be exactly 241 bytes");

constexpr uint16_t MAX_LIBRARY_BOOKS = 1000;
constexpr uint32_t INDEX_HEADER_SIZE = 12;

inline uint32_t offsetOf(uint16_t i) { return INDEX_HEADER_SIZE + i * (uint32_t)sizeof(CompactIndexRecord); }
