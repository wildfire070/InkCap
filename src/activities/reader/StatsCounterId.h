#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Zero identifies an existing legacy counter set. Fresh counters get an ID once
// at creation; copies, retries and restored snapshots retain that same ID.
struct StatsCounterId {
  std::array<uint8_t, 16> bytes{};
  static StatsCounterId create();
  void hex(char (&out)[33]) const {
    // Not named HEX: Arduino's Print.h #defines that to 16, which would
    // textually rewrite this array's declaration into a syntax error.
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
      out[i * 2] = kHexDigits[bytes[i] >> 4];
      out[i * 2 + 1] = kHexDigits[bytes[i] & 15];
    }
    out[32] = 0;
  }
};
