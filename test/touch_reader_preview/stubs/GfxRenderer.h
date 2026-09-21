#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFontFamily.h"

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  struct DrawCall {
    std::string text;
    int x = 0;
    int y = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    int tracking = 0;
  };

  mutable std::vector<DrawCall> drawCalls;

  int getLineHeight(int) const { return 10; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 1; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 1; }
  // Tracking is extra px between adjacent glyphs, as in the real renderer (never around a space).
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t tracking = 0) const { return tracking; }

  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style) const {
    return static_cast<int>(std::string(text).size()) * (fontId == 2 ? 2 : 1);
  }

  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, uint32_t, int8_t tracking = 0) const {
    const int glyphs = static_cast<int>(std::string(text).size());
    return getTextAdvanceX(fontId, text, style) + (glyphs > 1 ? (glyphs - 1) * tracking : 0);
  }

  void drawText(int, int x, int y, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, float = 1.0f, int8_t tracking = 0) const {
    drawCalls.push_back({text, x, y, style, tracking});
  }
};
