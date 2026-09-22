#pragma once

#include <strings.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "VectorFontSupport.h"

struct SdCardFontFileInfo {
  std::string path;   // v4 on-disk naming: "/<root>/<Family>/<Family>_<size>.cpfont"
                      // where <root> is "/.fonts" (preferred, hidden) or "/fonts" (visible).
                      // e.g. "/.fonts/NotoSansCJK/NotoSansCJK_14.cpfont"
  uint8_t pointSize;  // parsed from filename: 14 (0 for size-free vector fonts)
  uint8_t style;      // .cpfont: always 0 (all 4 styles bundled in one file).
                      // Vector family: the style ROLE of this file --
                      // 0=regular, 1=bold, 2=italic, 3=bold-italic.
};

struct SdCardFontFamilyInfo {
  // Names/range summaries do not hydrate paths. Detail consumers call this once.
  bool ensureDetails() const;
  // Drop paths loaded from the persistent index while retaining the family
  // summary. This lets streaming consumers bound RAM to one family at a time.
  void releaseDetails() const;
  uint8_t firstSize = 0, lastSize = 0;
  uint32_t indexOffset = 0, indexBytes = 0, indexHash = 0;
  uint16_t indexCount = 0;
  // Rebuild-only source marker. It is not serialized and is irrelevant after
  // the index has been loaded.
  bool sourceVisibleRoot = false;
  // true for a TrueType/OpenType family (loose .ttf/.otf/.ttc, or a folder of
  // them) rendered at any size via FreeInkFont/TtfEpdFont. Its `files` are
  // resident (never index-backed) with pointSize 0. Not serialized: vector
  // families are rediscovered by appendVectorFamilies() after every index load.
  bool vector = false;
  std::string name;  // directory name, e.g. "NotoSansCJK"
  mutable std::vector<SdCardFontFileInfo> files;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  const SdCardFontFileInfo* findClosestFile(uint8_t targetSize, uint8_t style = 0) const;
  std::vector<uint8_t> availableSizes() const;
};

// A filename weight token that collapses into the regular or bold role already covered by the plain
// Regular/Bold file. Google-style static families ship these as extra files (e.g. a Trial folder's
// -Thin/-Black), which the 4-role model (regular/bold/italic/bold-italic) never selects on its own.
inline bool vectorFileHasExtraWeightToken(const std::string& path) {
  const auto ciContains = [](const char* hay, const char* needle) {
    const size_t needleLen = std::strlen(needle);
    for (const char* p = hay; *p != '\0'; ++p) {
      if (strncasecmp(p, needle, needleLen) == 0) return true;
    }
    return false;
  };
  const size_t slash = path.rfind('/');
  const char* base = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  static const char* const kTokens[] = {"thin", "light", "black", "heavy", "extrabold", "ultrabold"};
  for (const char* token : kTokens) {
    if (ciContains(base, token)) return true;
  }
  return false;
}

// Drop extra-weight files (Light/Black/...) when a normal-weight sibling exists in the same
// upright/italic bucket -- so a family with a dozen static weights only scans and opens the four
// this model actually uses. The have-normal guard keeps every file when the family's own naming
// makes every candidate carry a weight token (so a folder with no plain "Regular"/"Bold" file is
// not emptied out). Called before each file is opened for inspection (style bit 1 = italic).
inline void dropExtraWeightVectorVariants(std::vector<SdCardFontFileInfo>& files) {
  for (const uint8_t italicBit : {uint8_t{0}, uint8_t{2}}) {
    bool haveNormal = false;
    for (const auto& info : files) {
      if ((info.style & 2) == italicBit && !vectorFileHasExtraWeightToken(info.path)) {
        haveNormal = true;
        break;
      }
    }
    if (!haveNormal) continue;
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const SdCardFontFileInfo& info) {
                                 return (info.style & 2) == italicBit && vectorFileHasExtraWeightToken(info.path);
                               }),
                files.end());
  }
}

// Point sizes offered for a scalable (TTF/OTF) family: every whole point from min to max.
inline constexpr uint8_t kVectorFontMinPointSize = 8;
inline constexpr uint8_t kVectorFontMaxPointSize = 16;

inline std::vector<uint8_t> vectorFontPointSizes() {
  std::vector<uint8_t> sizes;
  sizes.reserve(kVectorFontMaxPointSize - kVectorFontMinPointSize + 1);
  for (uint8_t s = kVectorFontMinPointSize; s <= kVectorFontMaxPointSize; ++s) sizes.push_back(s);
  return sizes;
}

// The offered step nearest to `target` (ties go to the smaller step); `target` itself when `sizes` is empty.
// `target` stays fixed while scanning -- comparing against the running best instead drifts to the first steps.
inline uint8_t closestPointSize(const std::vector<uint8_t>& sizes, const uint8_t target) {
  uint8_t best = target;
  uint8_t bestDiff = UINT8_MAX;
  bool found = false;
  for (const uint8_t candidate : sizes) {
    const uint8_t diff = candidate > target ? candidate - target : target - candidate;
    if (!found || diff < bestDiff || (diff == bestDiff && candidate < best)) {
      best = candidate;
      bestDiff = diff;
      found = true;
    }
  }
  return best;
}

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Two top-level roots are scanned at discovery time. Hidden is preferred
  // when creating new installs; both are read from if present.
  static constexpr const char* FONTS_DIR_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_VISIBLE = "/fonts";

  // Returns the existing root for `familyName` (the one that contains
  // /<root>/<familyName>/), or nullptr if the family is not installed in
  // either root. Used by writers to keep re-installs in their existing dir.
  static const char* findFamilyRoot(const char* familyName);

  // Returns the root path that should be used when creating a brand-new
  // family on disk (no prior install): the existing root if exactly one of
  // the two roots exists, otherwise the hidden root.
  static const char* defaultWriteRoot();

  // Scan SD card, populate families_. Returns true if any families found.
  // Use lastDiscoveryFailed() to distinguish an empty card from an incomplete
  // scan caused by a recoverable directory-entry allocation failure.
  bool discover();
  // Validate directory names/file sizes, then load names only from the index.
  // A missing/stale index is rebuilt once; font contents are not probed on hits.
  bool loadNames(bool checkInventory = false);
  static void invalidateIndex();
  bool needsRefresh() const;
  uint32_t revision() const { return revision_; }
  bool lastDiscoveryFailed() const { return discoveryFailed_; }
  void clear();

  // Parse a v4 .cpfont filename without allocating. Reused by the dictionary
  // font path, which scans one selected family without retaining a catalog.
  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  const SdCardFontFamilyInfo* findSummary(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically
  bool discoveryFailed_ = false;
  uint32_t revision_ = 0;
  uint32_t inventoryGeneration_ = 0;
  uint64_t inventoryFingerprint_ = 0;
  bool inventoryKnown_ = false;
  bool readIndex(uint64_t fingerprint);
#if CROSSPOINT_VECTOR_FONTS
  // Append loose/folder .ttf/.otf/.ttc families to families_ (cpfont names win).
  void appendVectorFamilies();
#endif

  // Rebuild the cache while retaining only family summaries and one directory
  // entry at a time. Full paths are written straight to the cache file.
  bool rebuildIndex(uint64_t fingerprint, uint32_t generation);
};
