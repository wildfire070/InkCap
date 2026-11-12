#ifndef AO3_LIBRARY_METADATA_H
#define AO3_LIBRARY_METADATA_H

#include <stdint.h>

#include <cstring>

#include "Ao3CompactIndexRecord.h"

/**
 * @brief Binary structure for AO3 sidecar metadata (v10).
 * Expanded to include core metadata for fast global library scanning.
 * Total size: ~2066 bytes.
 * Magic: 'AO3L'
 */
struct Ao3LibraryMetadata {
  char magic[4];                // 'AO3L'
  uint8_t version;              // 10
  char rating;                  // G, T, M, E, -
  char warning;                 // !, ?, -, B
  uint8_t isCompleted;          // 0 or 1
  uint8_t wordCountIsEstimate;  // 0 = author-reported, 1 = counted on-device
  uint32_t wordCount;
  uint16_t chapterCount;
  uint16_t totalChapters;  // 0 = unknown (e.g. AO3's own "N/?" for open-ended works)
  uint16_t seriesPart;
  char seriesName[128];
  char summary[512];
  // Display pills (Ao3LibraryActivity/Ao3PageQrActivity/Ao3EndOfBookSeriesActivity) still only ever
  // read/show the first 2-4 of these -- their own loops keep their own small hardcoded bounds. The
  // widened count/length here exists so every scraped tag is available for Library search (see
  // CompactIndexRecord::tags, which this is copied into wholesale at scrape time).
  char tags[AO3_SEARCH_TAG_COUNT][AO3_SEARCH_TAG_MAX_LEN];

  // Expanded for fast scanning
  char filepath[256];    // Absolute path to the .epub file
  char title[128];       // Book title
  char author[128];      // Book author
  char updatedDate[12];  // Last updated date YYYY-MM-DD

  Ao3LibraryMetadata() {
    magic[0] = 'A';
    magic[1] = 'O';
    magic[2] = '3';
    magic[3] = 'L';
    version = 10;
    rating = '-';
    warning = 0;
    isCompleted = 0;
    wordCountIsEstimate = 0;
    wordCount = 0;
    chapterCount = 0;
    totalChapters = 0;
    seriesPart = 0;
    memset(seriesName, 0, sizeof(seriesName));
    memset(summary, 0, sizeof(summary));
    memset(tags, 0, sizeof(tags));
    memset(filepath, 0, sizeof(filepath));
    memset(title, 0, sizeof(title));
    memset(author, 0, sizeof(author));
    memset(updatedDate, 0, sizeof(updatedDate));
  }

  bool isValid() const { return magic[0] == 'A' && magic[1] == 'O' && magic[2] == '3' && magic[3] == 'L'; }
};

#endif
