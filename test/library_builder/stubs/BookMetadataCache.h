#pragma once

#include <string>

// Minimal stand-in for the real lib/Epub/Epub/BookMetadataCache.h -- only the nested
// BookMetadata fields LibraryBuilder.cpp's AO3 piggyback-write logic actually reads.
class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string seriesName;
    std::string seriesIndex;
    std::string contentRating;
    std::string completionStatus;
    std::string wordCount;
    std::string fictags;
    std::string mainShip;
    std::string otherShips;
  };
};
