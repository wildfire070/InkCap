#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Just the progress shapes SyncParked.h stores and returns -- ClippingStore.cpp never touches
// these directly, this only exists to satisfy SyncParked.cpp's own include.
struct KOReaderRichPosition {
  uint32_t pctQ = 0;
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t totalPages = 1;
  std::optional<uint16_t> paragraphIndex;
  std::string xpath;
};

struct KOReaderProgress {
  std::string document;
  std::string progress;
  float percentage = 0;
  std::string device;
  std::string deviceId;
  int64_t timestamp = 0;
  std::optional<KOReaderRichPosition> position;
};

struct KOReaderProgressChange {
  static constexpr size_t MAX_ALIASES = 8;
  std::string document;
  std::string aliases[MAX_ALIASES];
  uint8_t aliasCount = 0;
  float percentage = 0;
  std::string progress;
  std::optional<KOReaderRichPosition> position;
  std::string device;
  int64_t timestamp = 0;
};
