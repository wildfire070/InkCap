#pragma once

#include <stdint.h>
#include <cstring>
#include <algorithm>
#include "Ao3CompactIndexRecord.h"

// FNV-1a 32-bit hash. Returns 0 for null/empty strings (sentinel for "no value").
inline uint32_t fnv1a(const char* str) {
    if (!str || str[0] == '\0') return 0;
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= static_cast<uint8_t>(*str++);
        hash *= 16777619u;
    }
    return hash;
}

/**
 * @brief In-RAM sort/filter key struct — one per live book, loaded sequentially
 *        from ao3_library_index.bin at library startup.
 *
 * 38 bytes packed (pragma pack 1).
 * 38 × 1000 books = 38 KB peak RAM.
 */
#pragma pack(push, 1)
struct ViewEntry {
    uint32_t cacheHash;      // same as CompactIndexRecord.cacheHash
    uint32_t wordCount;      // word count sort
    uint32_t seriesHash;     // fnv1a(seriesName), 0 if no series
    uint16_t addedSequence;  // date-added sort (monotonic, higher = newer), max 65535
    uint16_t seriesPart;     // position within series, 0 if not in a series
    char     title[12];      // first 11 chars of title, null-terminated (alphabetic sort)
    char     authorKey[8];   // first 7 chars of author lowercased (author sort)
    char     rating;         // same as CompactIndexRecord.rating (G, T, M, E, -)
    uint8_t  isCompleted;    // same as CompactIndexRecord.isCompleted
};
#pragma pack(pop)

/**
 * @brief Build a ViewEntry from a CompactIndexRecord at a known byte offset.
 */
inline ViewEntry buildViewEntry(const CompactIndexRecord& rec) {
    ViewEntry v{};

    strncpy(v.title,     rec.title,  11); v.title[11]     = '\0';

    // authorKey: first 7 chars, lowercased for case-insensitive sort
    strncpy(v.authorKey, rec.author, 7); v.authorKey[7] = '\0';
    for (char* p = v.authorKey; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }

    v.wordCount     = rec.wordCount;
    v.addedSequence = static_cast<uint16_t>(rec.addedSequence);
    v.seriesHash    = fnv1a(rec.seriesName);
    v.seriesPart    = rec.seriesPart;
    v.cacheHash     = rec.cacheHash;
    v.rating        = rec.rating;
    v.isCompleted   = rec.isCompleted;
    return v;
}
