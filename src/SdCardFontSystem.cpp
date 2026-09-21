#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {

struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

enum class FontFileSelection : uint8_t { Closest, Exact };

#if CROSSPOINT_VECTOR_FONTS
// Stable, non-zero renderer font id for a vector family at a size (FNV-1a of name + size, salted so it
// cannot collide with .cpfont ids). 0 is the "not found" sentinel, so bump collisions to 1.
int computeTtfFontId(const char* familyName, uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (const char* p = familyName; p && *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x54544600u;  // "TTF\0"
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}
#endif  // CROSSPOINT_VECTOR_FONTS

// This is a cold setup path, not a render loop. The 320-byte stack footprint
// (path and filename) replace a heap-allocated whole-font catalog
// during every dictionary swap, avoiding persistent fragmentation on the C3.
bool findInstalledFontFile(const char* familyName, const uint8_t targetPointSize, const FontFileSelection selection,
                           char* path, const size_t pathSize, uint8_t& selectedPointSize) {
  if (!familyName || familyName[0] == '\0' || !path || pathSize == 0) return false;

  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) return false;
  const int directoryLength = std::snprintf(path, pathSize, "%s/%s", root, familyName);
  if (directoryLength <= 0 || static_cast<size_t>(directoryLength) >= pathSize) return false;

  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;

  uint8_t closestSize = 0;
  uint8_t closestDiff = UINT8_MAX;
  char filename[128] = {};
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0) continue;

    if (selection == FontFileSelection::Closest) {
      const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
      if (closestDiff == UINT8_MAX || diff < closestDiff || (diff == closestDiff && pointSize < closestSize)) {
        closestSize = pointSize;
        closestDiff = diff;
      }
    }
  }
  dir.close();

  if (selection == FontFileSelection::Closest) {
    selectedPointSize = closestSize;
  } else if (selection == FontFileSelection::Exact) {
    selectedPointSize = targetPointSize;
  }

  if (selectedPointSize == 0) return false;
  // Scan once more to preserve the exact file name rather than assuming the
  // file base name matches the directory name.
  dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0 ||
        pointSize != selectedPointSize) {
      continue;
    }
    const int pathLength = std::snprintf(path, pathSize, "%s/%s/%s", root, familyName, filename);
    dir.close();
    return pathLength > 0 && static_cast<size_t>(pathLength) < pathSize;
  }
  dir.close();
  return false;
}

}  // namespace

// Out-of-line: TtfEpdFont must be a complete type for the unique_ptr members.
SdCardFontSystem::SdCardFontSystem() = default;
SdCardFontSystem::~SdCardFontSystem() = default;

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  if (SETTINGS.sdFontFamilyName[0] == '\0') {
    LOG_DBG("SDFS", "SD font resolver ready; discovery deferred until requested");
    return;
  }

  ensureLoaded(renderer);
  releaseRegistry();
}

void SdCardFontSystem::persistSettingsChange() const {
  if (settingsPersistenceCallback_) {
    settingsPersistenceCallback_(settingsPersistenceContext_);
  } else {
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  bool registryWasDirty = registryDirty_.load(std::memory_order_acquire) || fontReloadPending_ ||
                          loadedRegistryRevision_ != registry_.revision();

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  uint8_t targetPointSize = SETTINGS.getSdFontTargetPointSize();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      LOG_INF("SDFS", "No SD font wanted; unloading resident family: %s", currentFamily.c_str());
      manager_.unloadAll(renderer);
      loadedFontPointSize_ = 0;
    }
#if CROSSPOINT_VECTOR_FONTS
    if (!ttfFamily_.empty()) unloadTtf(renderer);
#endif
    return;
  }

#if CROSSPOINT_VECTOR_FONTS
  // Loaded TTF family at the wanted size and no disk change: nothing to do (avoids re-touching the registry).
  if (!registryWasDirty && ttf_ && ttfFamily_ == wantedFamily && ttfPointSize_ == targetPointSize) return;
#endif

  if (!registryWasDirty && currentFamily == wantedFamily && loadedFontPointSize_ == targetPointSize &&
      SETTINGS.legacySdFontSizeStep == UINT8_MAX) {
    LOG_INF("SDFS", "ensureLoaded: %s already resident at %u pt, no-op", wantedFamily, targetPointSize);
    return;
  }

  ensureRegistry();
  if (registry_.lastDiscoveryFailed()) return;
  registryWasDirty = registryWasDirty || fontReloadPending_ || loadedRegistryRevision_ != registry_.revision();

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && !family->ensureDetails()) return;
#if CROSSPOINT_VECTOR_FONTS
  // Vector (.ttf/.otf) families load through the FreeInkFont path; the .cpfont manager below would
  // reject them ("Invalid magic bytes") and wipe the user's selection.
  if (family && family->vector) {
    if (!currentFamily.empty()) manager_.unloadAll(renderer);
    loadTtfFamily(*family, renderer, registryWasDirty);
    fontReloadPending_ = false;
    loadedRegistryRevision_ = registry_.revision();
    return;
  }
  // Not on a vector family: release any previously loaded TTF before the .cpfont/built-in path takes over.
  if (!ttfFamily_.empty()) unloadTtf(renderer);
#endif
  if (family && SETTINGS.legacySdFontSizeStep != UINT8_MAX) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) {
      const uint8_t step = std::min<uint8_t>(SETTINGS.legacySdFontSizeStep, sizes.size() - 1);
      targetPointSize = sizes[step];
      SETTINGS.readerFontPointSize = targetPointSize;
      SETTINGS.legacySdFontSizeStep = UINT8_MAX;
      persistSettingsChange();
      LOG_INF("SDFS", "Migrated SD font size to %u pt", targetPointSize);
    }
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    if (!family) {
      LOG_ERR("SDFS", "SD font family disappeared: %s (clearing, falling back to built-in)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
      return;
    }
    const auto* wantedFile = family->findClosestFile(targetPointSize);
    uint8_t wantedPt = wantedFile ? wantedFile->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_INF("SDFS", "Reloading %s: size %u -> %u (target %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            targetPointSize, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  if (family) {
    if (manager_.loadFamilyClosest(*family, renderer, targetPointSize)) {
      loadedFontPointSize_ = targetPointSize;
      fontReloadPending_ = false;
      loadedRegistryRevision_ = registry_.revision();
      setupUiFallbacks(renderer);
      LOG_INF("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing, falling back to built-in)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
    }
  } else {
    LOG_ERR("SDFS", "SD font family not found: %s (clearing, falling back to built-in)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
  }
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  if (!ttfFamily_.empty()) {
    LOG_DBG("SDFS", "Released TTF font before low-memory operation: %s", ttfFamily_.c_str());
    unloadTtf(renderer);
  }
#endif
  if (manager_.currentFamilyName().empty()) return;

  const std::string familyName = manager_.currentFamilyName();
  (void)familyName;
  manager_.unloadAll(renderer);
  loadedFontPointSize_ = 0;
  LOG_DBG("SDFS", "Released SD card font before low-memory operation: %s", familyName.c_str());
}

void SdCardFontSystem::ensureRegistry() {
  const bool dirty = registryDirty_.exchange(false, std::memory_order_acq_rel);
  if (dirty) fontReloadPending_ = true;
  if (registryLoaded_ && !dirty && !registry_.needsRefresh()) return;
  if (dirty) LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
  registry_.loadNames();
  if (registry_.lastDiscoveryFailed()) {
    LOG_ERR("SDFS", "SD font registry scan ran out of memory (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    registryDirty_.store(true, std::memory_order_release);
    return;
  }
  registryLoaded_ = true;
}

void SdCardFontSystem::releaseRegistry() {
  if (!registryLoaded_) return;
  LOG_DBG("SDFS", "Releasing SD font catalog (%d families)", registry_.getFamilyCount());
  registry_.clear();
  registryLoaded_ = false;
}

void SdCardFontSystem::releaseForNetwork(GfxRenderer& renderer) {
  releaseLoadedFont(renderer);

  releaseRegistry();
  registryDirty_.store(true, std::memory_order_release);
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // See SdCardFontManager::unloadAll()'s comment: render() reads fontMap/
  // fallbackFontMap_ unlocked on the render task's side, so this lookup and
  // the setFallbackFont() writes below need the same guard. Scoped tightly
  // around just those two touches -- NOT around loadFamilyExtraSize()'s SD
  // reads below, which would otherwise stall the render task for the
  // duration of up to kUiFontSizes' worth of file loads (loadFilePath()
  // already guards its own map mutation internally).
  bool hasCjk = false;
  {
    GfxRenderer::MutexGuard guard(renderer);
    const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
    if (readerIt == renderer.getFontMap().end()) return;
    static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
    for (const uint32_t cp : kCjkProbes) {
      if (readerIt->second.hasCodepoint(cp)) {
        hasCjk = true;
        break;
      }
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      GfxRenderer::MutexGuard guard(renderer);
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

void SdCardFontSystem::setupUiFallbacksDirect(GfxRenderer& renderer, const char* familyName) {
  if (!familyName || familyName[0] == '\0') return;

  // See setupUiFallbacks() above -- same tight scoping rationale.
  bool hasCjk = false;
  {
    GfxRenderer::MutexGuard guard(renderer);
    const auto readerIt = renderer.getFontMap().find(manager_.getFontId(manager_.currentFamilyName()));
    if (readerIt == renderer.getFontMap().end()) return;
    static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
    for (const uint32_t cp : kCjkProbes) {
      if (readerIt->second.hasCodepoint(cp)) {
        hasCjk = true;
        break;
      }
    }
  }
  if (!hasCjk) return;

  for (const auto& ui : kUiFontSizes) {
    char path[160] = {};
    uint8_t pointSize = 0;
    if (!findInstalledFontFile(familyName, ui.pointSize, FontFileSelection::Exact, path, sizeof(path), pointSize)) {
      continue;
    }
    const int sdFontId = manager_.loadFamilyExtraFile(path, familyName, pointSize, renderer);
    if (sdFontId != 0) {
      GfxRenderer::MutexGuard guard(renderer);
      renderer.setFallbackFont(ui.fontId, sdFontId);
    }
  }
}

#if CROSSPOINT_VECTOR_FONTS

void SdCardFontSystem::freeTtfSources() {
  for (auto& s : ttfSources_) {
    s.bytes.clear();
    freeink::font::PsramVector<uint8_t>().swap(s.bytes);  // actually release
    if (s.file) s.file.close();
    s.streamed = false;
    s.size = 0;
    s.present = false;
  }
}

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  if (ttfFamily_.empty() && ttfFontId_ == 0 && ttfUiIds_.empty()) return;
  {
    // render() reads fontMap/fallbackFontMap_ unlocked on the render task's side, so removing entries
    // needs the same guard the .cpfont manager uses. Scoped to the map mutation only.
    GfxRenderer::MutexGuard guard(renderer);
    for (const int id : ttfUiIds_) {  // UI-size fallbacks first (they borrow ttfSources_)
      renderer.unregisterTtfFont(id);
      renderer.removeFont(id);
    }
    renderer.clearFallbackFonts();
    if (ttfFontId_ != 0) {
      renderer.unregisterTtfFont(ttfFontId_);
      renderer.removeFont(ttfFontId_);
    }
  }
  ttfUiIds_.clear();
  ttfUi_.clear();
  ttf_.reset();  // frees the FT faces first (they read ttfSources_)
  freeTtfSources();
  ttfFamily_.clear();
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

unsigned long SdCardFontSystem::ttfRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count) {
  auto* f = static_cast<HalFile*>(ctx);
  if (f == nullptr || !*f) return 0;
  if (!f->seek(static_cast<size_t>(offset))) return 0;
  const int n = f->read(buffer, count);
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

bool SdCardFontSystem::openTtfSource(const uint8_t style, const std::string& path) {
  if (style >= 4) return false;
  // Small fonts are read fully into RAM (fastest, fewest SD reads; PSRAM when present). Large fonts STREAM
  // from SD so a multi-MB variable/CJK file never sits in RAM; the handle is kept open for the font's life.
  static constexpr size_t kResidentMax = 6 * 1024 * 1024;
  // Working headroom that must remain in internal DRAM after a resident load.
  static constexpr size_t kInternalHeadroom = 96 * 1024;
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    LOG_ERR("SDFS", "Failed to open TTF: %s", path.c_str());
    return false;
  }
  const size_t len = f.size();
  if (len == 0) {
    LOG_ERR("SDFS", "Empty TTF: %s", path.c_str());
    f.close();
    return false;
  }
  TtfSource& s = ttfSources_[style];
  // PsramAlloc aborts on OOM, so this gate is load-bearing: fall back to streaming instead of attempting
  // an allocation that can fail.
  bool resident = len <= kResidentMax;
  if (resident && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < len) {
    const size_t internalFree = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internalFree < len + kInternalHeadroom) {
      LOG_DBG("SDFS", "TTF %s (%u KB) too large for DRAM (largest block %u KB), streaming", path.c_str(),
              static_cast<unsigned>(len / 1024), static_cast<unsigned>(internalFree / 1024));
      resident = false;
    }
  }
  if (resident) {
    s.bytes.resize(len);
    const int got = f.read(s.bytes.data(), len);
    f.close();
    if (static_cast<size_t>(got) != len) {
      LOG_ERR("SDFS", "Short read on TTF %s (%d/%u)", path.c_str(), got, static_cast<unsigned>(len));
      s.bytes.clear();
      return false;
    }
    s.streamed = false;
  } else {
    s.file = std::move(f);  // kept open; ttfRead() reads it on demand
    s.streamed = true;
    LOG_DBG("SDFS", "Streaming TTF %s (%u KB) from SD", path.c_str(), static_cast<unsigned>(len / 1024));
  }
  s.size = static_cast<unsigned long>(len);
  s.present = true;
  return true;
}

void SdCardFontSystem::addTtfSources(TtfEpdFont& font) {
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = ttfSources_[st];
    if (!s.present) continue;
    if (s.streamed) {
      font.addStreamSource(st, &SdCardFontSystem::ttfRead, &s.file, s.size);
    } else {
      font.addResidentSource(st, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
    }
  }
}

void SdCardFontSystem::setupTtfUiFallbacks(GfxRenderer& renderer) {
  if (ttfFamily_.empty()) return;
  // Small caches: UI strings are short. Each UI family is 4-style but LAZY, so only the regular face is
  // ever built for UI text. All faces share the reader's sources, so no extra copy of any font file.
  // Without PSRAM each fallback competes with the reader's section build for internal DRAM, and the build
  // must win: below this floor, skip the fallback (built-in bitmap UI fonts keep covering Latin UI text).
  static constexpr size_t kUiFallbackMinInternalHeap = 160 * 1024;
  for (const auto& ui : kUiFontSizes) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) == 0) {
      const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (internalFree < kUiFallbackMinInternalHeap) {
        LOG_DBG("SDFS", "Skipping TTF UI fallback @%upt (%u KB internal free)", ui.pointSize,
                static_cast<unsigned>(internalFree / 1024));
        continue;
      }
    }
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for UI fallback @%upt", ui.pointSize);
      continue;
    }
    addTtfSources(*f);
    if (!f->load(ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384)) continue;
    // Distinct id from the reader-size font (a UI size can equal the reader size): salt the family name.
    const int id = computeTtfFontId((ttfFamily_ + "\x01ui").c_str(), ui.pointSize);
    {
      GfxRenderer::MutexGuard guard(renderer);
      renderer.insertFont(id, f->family());
      renderer.registerTtfFont(id, f.get());
      renderer.setFallbackFont(ui.fontId, id);
    }
    ttfUiIds_.push_back(id);
    ttfUi_.push_back(std::move(f));
  }
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool registryWasDirty) {
  // Vector fonts render at any size; snap the reader size onto the family's offered steps.
  const uint8_t size = closestPointSize(family.availableSizes(), SETTINGS.getSdFontTargetPointSize());
  if (size != SETTINGS.readerFontPointSize) {
    SETTINGS.readerFontPointSize = size;
    persistSettingsChange();
  }

  // Already loaded, same family + size, and disk unchanged: nothing to do.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name && ttfPointSize_ == size) return;

  unloadTtf(renderer);

  if (family.files.empty()) {
    LOG_ERR("SDFS", "Vector family %s has no file", family.name.c_str());
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
    return;
  }

  // Open each style source the family ships (0=regular, 1=bold, 2=italic, 3=bold-italic). A single-file
  // family supplies only regular; TtfEpdFont then derives bold/italic from the wght axis or an oblique
  // shear. Extra files upgrade those styles to the real designs.
  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (ttfSources_[role].present) continue;  // registry already deduped by role
    openTtfSource(role, file.path);
  }
  if (!ttfSources_[0].present) {
    // Possibly a transient SD read failure: keep the selection so the next ensureLoaded() retries.
    LOG_ERR("SDFS", "Vector family %s: regular file failed to open (keeping selection)", family.name.c_str());
    freeTtfSources();
    return;
  }

  ttf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf_) {
    LOG_ERR("SDFS", "OOM: TtfEpdFont for %s", family.name.c_str());
    freeTtfSources();
    return;
  }
  addTtfSources(*ttf_);
  if (!ttf_->load(size)) {
    // Ambiguous (corrupt font vs. transient OOM inside FreeType): keep the selection and retry next time.
    LOG_ERR("SDFS", "FreeInkFont could not parse %s (keeping selection)", family.name.c_str());
    ttf_.reset();
    freeTtfSources();
    return;
  }
  ttf_->build(" ");  // seed the regular face; other styles and glyphs fault in on demand

  ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
  {
    GfxRenderer::MutexGuard guard(renderer);
    renderer.insertFont(ttfFontId_, ttf_->family());
    renderer.registerTtfFont(ttfFontId_, ttf_.get());
  }
  ttfFamily_ = family.name;
  ttfPointSize_ = size;
  LOG_INF("SDFS", "Loaded TTF font: %s @ %upt (id %d, heap free %u, max block %u)", family.name.c_str(), size,
          ttfFontId_, static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  setupTtfUiFallbacks(renderer);
}

#endif  // CROSSPOINT_VECTOR_FONTS

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager loads exactly one size (closest to the selected point size), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
#if CROSSPOINT_VECTOR_FONTS
  // A loaded vector (.ttf) family answers first: it is not in the .cpfont manager.
  if (ttfFontId_ != 0 && familyName && ttfFamily_ == familyName) return ttfFontId_;
#endif
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    refreshIfDirty();
    if (registry_.lastDiscoveryFailed()) return false;
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (!family->ensureDetails()) return false;
      const auto sizes = family->availableSizes();
      if (changeReaderFontSizeStep(sizes.data(), sizes.size(), SETTINGS.readerFontPointSize, larger, mode)) return true;
      if (sizes.size() > 0) return false;
    }
  }

  return SETTINGS.changeReaderFontSize(larger, mode);
}

uint8_t SdCardFontSystem::resolveLegacySizeStep(const char* familyName, const uint8_t sizeStep) {
  ensureRegistry();
  const auto* family = familyName ? registry_.findFamily(familyName) : nullptr;
  if (family) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) return sizes[std::min<uint8_t>(sizeStep, sizes.size() - 1)];
  }
  return CrossPointSettings::getSdFontRangePointSize(SETTINGS.sdFontSizeRange, sizeStep);
}

DictionaryFontActivation SdCardFontSystem::activateDictionaryFont(GfxRenderer& renderer, const char* familyName,
                                                                  uint8_t targetPointSize) {
  // A non-zero size with no dedicated family means "use the reader's installed
  // family at this size". This keeps the setting useful when the same custom
  // family is wanted for reading and definitions without keeping two families
  // resident.
  if ((!familyName || familyName[0] == '\0') && targetPointSize != 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    familyName = SETTINGS.sdFontFamilyName;
  }
  if (!familyName || familyName[0] == '\0') {
    return {restoreReaderFont(renderer), false};
  }

  MemoryBudget::logHeapShape("dict.font_before_activate");
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  // Prefer the actual loaded reader-file size. Built-in readers have no SD
  // file, so use their effective physical point size instead.
  if (targetPointSize == 0) {
    targetPointSize = manager_.currentPointSize() != 0
                          ? manager_.currentPointSize()
                          : CrossPointSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
  }
  if (!findInstalledFontFile(familyName, targetPointSize, FontFileSelection::Closest, path, sizeof(path),
                             selectedPointSize)) {
    LOG_DBG("SDFS", "Dictionary font not found on card: %s", familyName);
    const char* globalFamilyName = SETTINGS.dictionarySdFontFamilyName;
    if (globalFamilyName[0] != '\0' && std::strcmp(familyName, globalFamilyName) != 0) {
      LOG_DBG("SDFS", "Using global dictionary font while per-book font is unavailable: %s", globalFamilyName);
      return activateDictionaryFont(renderer, globalFamilyName, SETTINGS.dictionaryFontPointSize);
    }
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_reader_fallback");
    return {readerFontId, false};
  }

  if (manager_.currentFamilyName() == familyName && manager_.currentPointSize() == selectedPointSize) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    MemoryBudget::logHeapShape("dict.font_reused_reader");
    return {fontId, true};
  }

  // A reader SD font can retain page glyphs, kerning, and advance tables after
  // a long reading session. They are disposable at this handoff: keeping them
  // through the headroom check makes a dictionary font appear unavailable until
  // its book cache is deleted or the heap happens to be less fragmented.
  const int activeReaderFontId = SETTINGS.getReaderFontId();
  const auto beforeCacheRelease = MemoryBudget::snapshot();
  if (renderer.releaseSdCardFontForLowMemory(activeReaderFontId)) {
    const auto afterCacheRelease = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader SD-font caches before dictionary swap: free=%u->%u maxAlloc=%u->%u",
            beforeCacheRelease.freeHeap, afterCacheRelease.freeHeap, beforeCacheRelease.maxAllocHeap,
            afterCacheRelease.maxAllocHeap);
  }

  auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    // The reader family itself is also disposable for a dictionary swap. Retry
    // after releasing it so its font data does not cause a false low-memory
    // fallback.
    const auto beforeReaderUnload = heap;
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    heap = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader font before dictionary swap retry: free=%u->%u maxAlloc=%u->%u",
            beforeReaderUnload.freeHeap, heap.freeHeap, beforeReaderUnload.maxAllocHeap, heap.maxAllocHeap);
  }
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    LOG_ERR("SDFS", "Low heap for dictionary font swap (%u free, %u max alloc, need %u/%u); using reader font",
            heap.freeHeap, heap.maxAllocHeap, MemoryBudget::DICTIONARY_SD_FONT_MIN_FREE,
            MemoryBudget::DICTIONARY_SD_FONT_MIN_MAX_ALLOC);
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_heap_fallback");
    return {readerFontId, false};
  }

  // unloadAll() also drops optional CJK UI sizes before the dictionary file is
  // allocated, so both families are never resident at once.
  if (!manager_.currentFamilyName().empty()) {
    manager_.unloadAll(renderer);
  }
  loadedFontPointSize_ = 0;

  if (manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    LOG_DBG("SDFS", "Activated dictionary font %s at %u pt", familyName, manager_.currentPointSize());
    MemoryBudget::logHeapShape("dict.font_after_activate");
    return {fontId, true};
  }

  LOG_ERR("SDFS", "Failed to load dictionary font %s; restoring reader font", familyName);
  const int readerFontId = restoreReaderFont(renderer);
  MemoryBudget::logHeapShape("dict.font_reader_fallback");
  return {readerFontId, false};
}

int SdCardFontSystem::restoreReaderFont(GfxRenderer& renderer) {
  const char* familyName = SETTINGS.sdFontFamilyName;
  if (!familyName || familyName[0] == '\0') {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return SETTINGS.getBuiltInReaderFontId();
  }

  char path[160] = {};
  uint8_t selectedPointSize = 0;
  if (!findInstalledFontFile(familyName, SETTINGS.getSdFontTargetPointSize(), FontFileSelection::Closest, path,
                             sizeof(path), selectedPointSize)) {
    LOG_ERR("SDFS", "Reader font unavailable while restoring: %s", familyName);
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return SETTINGS.getBuiltInReaderFontId();
  }

  if (manager_.currentFamilyName() != familyName || manager_.currentPointSize() != selectedPointSize) {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    if (!manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
      LOG_ERR("SDFS", "Failed to restore reader font: %s", familyName);
      MemoryBudget::logHeapShape("dict.font_after_restore");
      return SETTINGS.getBuiltInReaderFontId();
    }
    loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
    setupUiFallbacksDirect(renderer, familyName);
  }

  const int fontId = manager_.getFontId(manager_.currentFamilyName());
  MemoryBudget::logHeapShape("dict.font_after_restore");
  return fontId != 0 ? fontId : SETTINGS.getBuiltInReaderFontId();
}

void SdCardFontSystem::markRegistryDirtyForPath(const char* path) {
  if (!path) return;
  for (const char* root : {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE}) {
    const size_t length = std::strlen(root);
    if (strncasecmp(path, root, length) == 0 && (path[length] == '/' || path[length] == '\0')) {
      markRegistryDirty();
      return;
    }
  }
}
