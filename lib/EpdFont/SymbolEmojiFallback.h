#pragma once
#include <cstdint>

#include "EpdFont.h"

// Optional, SD-card-loaded fallback glyph source for symbols/emoji that the
// built-in UI and reader fonts don't carry. Scoped to CROSSINK_SCALABLE_FONTS
// boards (PSRAM, SD-font-capable): on other boards ensureLoaded() always
// returns nullptr and callers remain unaffected.
namespace SymbolEmojiFallback {
// Returns the regular-style glyph source at the given point size, or nullptr
// if the font file isn't present on the SD card (or this board has no
// SD-font support). The underlying font file is opened at most once per
// boot; each distinct point size requested is rasterized lazily and cached
// by the scalable-font layer. Safe to call repeatedly.
const EpdFont* ensureLoaded(uint8_t pointSize);
}  // namespace SymbolEmojiFallback
