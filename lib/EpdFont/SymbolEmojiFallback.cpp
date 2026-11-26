#include "SymbolEmojiFallback.h"

#if CROSSINK_SCALABLE_FONTS
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>

#include <memory>

namespace {
constexpr const char* kFallbackFontPath = "/fonts/_system/NotoEmoji.ttf";
}  // namespace

namespace SymbolEmojiFallback {
const EpdFont* ensureLoaded() {
  static bool attempted = false;
  static std::unique_ptr<SdCardFont> font;
  if (attempted) return font ? font->getEpdFont(0) : nullptr;
  attempted = true;
  if (!Storage.exists(kFallbackFontPath)) return nullptr;
  auto candidate = makeUniqueNoThrow<SdCardFont>();
  if (!candidate || !candidate->load(kFallbackFontPath)) {
    LOG_ERR("SYMFB", "Failed to load symbol/emoji fallback font %s", kFallbackFontPath);
    return nullptr;
  }
  font = std::move(candidate);
  LOG_INF("SYMFB", "Loaded symbol/emoji fallback font %s", kFallbackFontPath);
  return font->getEpdFont(0);
}
}  // namespace SymbolEmojiFallback

#else

namespace SymbolEmojiFallback {
const EpdFont* ensureLoaded() { return nullptr; }
}  // namespace SymbolEmojiFallback

#endif
