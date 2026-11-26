#pragma once
#include "EpdFont.h"

// Optional, SD-card-loaded fallback glyph source for symbols/emoji that the
// built-in UI and reader fonts don't carry. Scoped to CROSSINK_SCALABLE_FONTS
// boards (PSRAM, SD-font-capable): on other boards ensureLoaded() always
// returns nullptr and callers remain unaffected.
namespace SymbolEmojiFallback {
// Returns the regular-style glyph source, or nullptr if the font file isn't
// present on the SD card (or this board has no SD-font support). The load is
// attempted at most once per boot; safe to call repeatedly.
const EpdFont* ensureLoaded();
}  // namespace SymbolEmojiFallback
