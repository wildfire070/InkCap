#include "DictionaryWordMeasure.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"

namespace DictionaryWordMeasure {

namespace {
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
}  // namespace

const char* withoutSoftHyphens(const char* word, const size_t length, char* scratch, const size_t scratchCapacity) {
  if (!word || !scratch || scratchCapacity == 0) return word;
  bool hasSoftHyphen = false;
  for (size_t i = 0; i + 1 < length; ++i) {
    if (word[i] == SOFT_HYPHEN_UTF8[0] && word[i + 1] == SOFT_HYPHEN_UTF8[1]) {
      hasSoftHyphen = true;
      break;
    }
  }
  if (!hasSoftHyphen) return word;

  size_t out = 0;
  for (size_t i = 0; i < length && out + 1 < scratchCapacity;) {
    if (i + 1 < length && word[i] == SOFT_HYPHEN_UTF8[0] && word[i + 1] == SOFT_HYPHEN_UTF8[1]) {
      i += SOFT_HYPHEN_BYTES;
      continue;
    }
    scratch[out++] = word[i++];
  }
  scratch[out] = '\0';
  return scratch;
}

int16_t measureWordAdvanceX(const GfxRenderer& renderer, const int fontId, const char* word, const size_t length,
                            const EpdFontFamily::Style style, char* scratch, const size_t scratchCapacity) {
  const char* measured = withoutSoftHyphens(word, length, scratch, scratchCapacity);
  return static_cast<int16_t>(renderer.getTextAdvanceX(
      fontId, measured, style, 0, CrossPointSettings::characterSpacingLevel(SETTINGS.characterSpacing)));
}

int16_t measureWordAdvanceX(const GfxRenderer& renderer, const int fontId, const char* word, const size_t length,
                            const EpdFontFamily::Style style, const uint8_t focusBoundary, const uint16_t focusSuffixX,
                            char* scratch, const size_t scratchCapacity) {
  if (focusBoundary == 0 || focusSuffixX == 0) {
    return measureWordAdvanceX(renderer, fontId, word, length, style, scratch, scratchCapacity);
  }
  const size_t suffixStart = std::min<size_t>(focusBoundary, length);
  return static_cast<int16_t>(
      focusSuffixX + renderer.getTextAdvanceX(fontId, word + suffixStart, style, 0,
                                              CrossPointSettings::characterSpacingLevel(SETTINGS.characterSpacing)));
}

int16_t measureWordAdvanceX(const GfxRenderer& renderer, const int fontId, const char* word, const size_t length,
                            const EpdFontFamily::Style style, const uint8_t focusBoundary,
                            const uint16_t focusRunOffset, const bool wordIsRtl, char* scratch,
                            const size_t scratchCapacity) {
  if (!wordIsRtl || focusBoundary == 0 || focusRunOffset == 0) {
    return measureWordAdvanceX(renderer, fontId, word, length, style, focusBoundary, focusRunOffset, scratch,
                               scratchCapacity);
  }

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  char boldBuf[40];
  size_t boldLen = std::min<size_t>({static_cast<size_t>(focusBoundary), length, sizeof(boldBuf) - 1});
  // The clamp to sizeof(boldBuf)-1 can land mid-UTF-8-sequence even though
  // focusBoundary itself was chosen to be safe within the unclamped word --
  // trim back to the last complete codepoint.
  boldLen = static_cast<size_t>(utf8SafeTruncateBuffer(word, static_cast<int>(boldLen)));
  memcpy(boldBuf, word, boldLen);
  boldBuf[boldLen] = '\0';
  return static_cast<int16_t>(
      focusRunOffset + renderer.getTextAdvanceX(fontId, boldBuf, boldStyle, 0,
                                                CrossPointSettings::characterSpacingLevel(SETTINGS.characterSpacing)));
}

}  // namespace DictionaryWordMeasure
