#include "SymbolEmojiFallback.h"

#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#include <HalStorage.h>
#include <Logging.h>

#include <optional>

namespace {
constexpr const char* kFallbackFontPath = "/fonts/_system/NotoEmoji.ttf";
enum class State : uint8_t { kUntried, kUnavailable, kReady };
State state = State::kUntried;
std::optional<HalScalableFont> face;
}  // namespace

namespace SymbolEmojiFallback {
const EpdFont* ensureLoaded(const uint8_t pointSize) {
  if (state == State::kUntried) {
    state = State::kUnavailable;
    if (Storage.exists(kFallbackFontPath)) {
      face.emplace();
      if (face->openFile(kFallbackFontPath)) {
        state = State::kReady;
        LOG_INF("SYMFB", "Loaded symbol/emoji fallback font %s", kFallbackFontPath);
      } else {
        LOG_ERR("SYMFB", "Failed to load symbol/emoji fallback font %s", kFallbackFontPath);
        face.reset();
      }
    }
  }
  if (state != State::kReady) return nullptr;
  return face->atSize(pointSize);
}
}  // namespace SymbolEmojiFallback

#else

namespace SymbolEmojiFallback {
const EpdFont* ensureLoaded(uint8_t) { return nullptr; }
}  // namespace SymbolEmojiFallback

#endif
