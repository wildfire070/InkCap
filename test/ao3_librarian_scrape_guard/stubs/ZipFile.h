#pragma once

#include <cstddef>
#include <cstdint>

// Minimal stub: only fnvHash64 is used by Ao3Librarian.cpp. Copied verbatim from the real
// lib/ZipFile/ZipFile.h (standard FNV-1a, no dependencies) so hash values match production.
class ZipFile {
 public:
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }
};
