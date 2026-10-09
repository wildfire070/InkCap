#pragma once

#include <EpdFontFamily.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Pure word-width measurement used by DictionaryWordSelectActivity::extractWords() to build
// tap targets that match what the page actually drew. Extracted out of
// DictionaryWordSelectActivity.cpp (where these lived in an anonymous namespace, invisible
// outside that translation unit) so they're host-testable without pulling in the whole Activity
// and its FreeRTOS/MappedInputManager/WordSelectNavigator dependencies -- these functions take
// only a renderer and primitives, no Activity state.
namespace DictionaryWordMeasure {

// Strips soft hyphens (U+00AD) before measuring, mirroring ParsedText's own layout-time
// stripping (ParsedText.cpp) -- otherwise a measured word's width includes a glyph the renderer
// never draws, and the highlight box overruns into the next word's gap. `scratch`/`scratchCapacity`
// are caller-owned working memory; returns `word` itself unchanged when no soft hyphen is present.
const char* withoutSoftHyphens(const char* word, size_t length, char* scratch, size_t scratchCapacity);

// Plain measurement: the word's rendered advance, reading the current global
// character-spacing level (CrossPointSettings::characterSpacingLevel) internally.
int16_t measureWordAdvanceX(const GfxRenderer& renderer, int fontId, const char* word, size_t length,
                            EpdFontFamily::Style style, char* scratch, size_t scratchCapacity);

// Focus-reading overload: for a word with a bold/plain split at `focusBoundary`, measures only
// the un-highlighted suffix past `focusSuffixX` (the highlighted prefix's width is already known
// and passed in), falling back to the plain overload when there is no focus split.
int16_t measureWordAdvanceX(const GfxRenderer& renderer, int fontId, const char* word, size_t length,
                            EpdFontFamily::Style style, uint8_t focusBoundary, uint16_t focusSuffixX, char* scratch,
                            size_t scratchCapacity);

// RTL focus-reading overload: an RTL word's bold prefix is measured as an actual bold run
// (RTL glyph shaping is direction-dependent, so the LTR "prefix width, then add the rest"
// shortcut the other overload uses does not hold) via a bounded local stack buffer, truncated to
// the last complete UTF-8 codepoint.
int16_t measureWordAdvanceX(const GfxRenderer& renderer, int fontId, const char* word, size_t length,
                            EpdFontFamily::Style style, uint8_t focusBoundary, uint16_t focusRunOffset, bool wordIsRtl,
                            char* scratch, size_t scratchCapacity);

}  // namespace DictionaryWordMeasure
