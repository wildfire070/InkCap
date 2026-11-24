#pragma once
#include <cstdint>
// ESP's complemented CRC32-LE API has the same seed/result convention as
// OptimizerFormat::crc()'s own portable fallback below -- reuse that
// algorithm directly rather than pulling in system zlib for this host tool.
inline uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t* data, uint32_t length) {
  uint32_t value = ~seed;
  while (length--) {
    value ^= *data++;
    for (int i = 0; i < 8; ++i) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1)));
  }
  return ~value;
}
