#pragma once

#include <Memory.h>

#include <cstdint>
#include <string>

// A book's spine item, pre-resolved once per reading session instead of on
// every chapter open. Plain POD (no ZipFile type dependency: ZipFile.h
// includes this header, not the other way around). Sorted ascending by
// (hrefHash, hrefLen) for loadFileStatSlim's binary search.
struct SpineZipStat {
  uint64_t hrefHash = 0;
  uint16_t hrefLen = 0;
  uint16_t method = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t localHeaderOffset = 0;
};

// Owned by one opened book, never shared globally across reader sessions.
// Only the ZIP directory is retained; entry names are always compared in full.
struct ZipDirectoryCache {
  HeapByteBuffer bytes;
  std::string path;
  size_t fileSize = 0;
  uint32_t modified = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
  uint16_t count = 0;
  bool attempted = false;

  // Populated once per reading session (Epub::prewarmSpineZipStats) on boards
  // without a PSRAM directory cache, where every chapter/image open would
  // otherwise fall through to a resume-from-last-position SD scan that
  // degrades badly when the zip interleaves images with spine entries. Left
  // empty (and simply skipped by loadFileStatSlim) if the heap guard refuses
  // the allocation or the book has no PSRAM-independent benefit from it.
  std::unique_ptr<SpineZipStat[]> spineStats;
  uint16_t spineStatCount = 0;
  bool spineStatsAttempted = false;

  void clear() {
    bytes.reset();
    path.clear();
    attempted = false;
    spineStats.reset();
    spineStatCount = 0;
    spineStatsAttempted = false;
  }
};
