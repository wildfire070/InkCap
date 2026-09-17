#pragma once

#include <cstdint>

#include "FontCacheManager.h"

class GfxRenderer {
 public:
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
  FontCacheManager* getFontCacheManager() { return nullptr; }
};
