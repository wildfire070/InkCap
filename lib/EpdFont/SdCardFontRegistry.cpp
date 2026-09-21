#include "SdCardFontRegistry.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "FontCatalogIndex.h"

#if CROSSPOINT_VECTOR_FONTS
#include <FtFont.h>
#include <strings.h>
#endif

namespace {
std::atomic<uint32_t> indexGeneration{0};
}

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  if (!ensureDetails()) return nullptr;
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findClosestFile(uint8_t targetSize, uint8_t style) const {
  if (!ensureDetails()) return nullptr;
  const SdCardFontFileInfo* best = nullptr;
  uint8_t bestDiff = UINT8_MAX;
  for (const auto& f : files) {
    if (f.style != style) continue;
    const uint8_t diff = f.pointSize > targetSize ? f.pointSize - targetSize : targetSize - f.pointSize;
    if (!best || diff < bestDiff || (diff == bestDiff && f.pointSize < best->pointSize)) {
      best = &f;
      bestDiff = diff;
    }
  }
  return best;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
#if CROSSPOINT_VECTOR_FONTS
  // Scalable outlines render at any size: offer every whole point in range.
  if (vector) return vectorFontPointSizes();
#endif
  if (!ensureDetails()) return {};
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  // V4 .cpfont files bundle every style (regular/bold/italic/bold-italic) into
  // one file, so style is always 0 at the registry level. The per-style
  // bitstream is selected later by SdCardFont::getEpdFont(style). The `style`
  // field in SdCardFontFileInfo is reserved for future formats that split
  // styles across files; catalog scans defend against accidental duplicates.
  style = 0;
  return true;
}

namespace {
bool scanFamilySummary(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    const bool ok = !dir.allocationFailed();
    dir.close();
    return ok;
  }

  // Point sizes are uint8_t, so a 32-byte bitmap rejects duplicate files
  // without retaining their paths.
  uint8_t seenSizes[32] = {};
  char name[128];
  uint16_t count = 0;
  uint8_t first = UINT8_MAX, last = 0;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(name, sizeof(name));
    entry.close();
    if (isDirectory || name[0] == '.' || name[0] == '_') continue;

    uint8_t size = 0, style = 0;
    if (!SdCardFontRegistry::parseFilename(name, size, style)) continue;
    const size_t pathLength = std::strlen(dirPath) + 1 + std::strlen(name);
    if (pathLength > fontcatalog::MaxPath) continue;
    const uint8_t mask = uint8_t(1U << (size & 7U));
    if (seenSizes[size >> 3U] & mask) {
      LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", name, dirPath);
      continue;
    }
    seenSizes[size >> 3U] |= mask;
    first = std::min(first, size);
    last = std::max(last, size);
    ++count;
  }
  const bool ok = !FsHelpers::directoryIterationFailed(dir);
  dir.close();
  if (!ok) return false;
  family.indexCount = count;
  family.firstSize = count ? first : 0;
  family.lastSize = last;
  return true;
}

bool addFamilySummaries(const char* rootPath, bool visibleRoot, std::vector<SdCardFontFamilyInfo>& families) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    const bool ok = !root.allocationFailed();
    root.close();
    return ok;
  }
  if (!root.isDirectory()) {
    root.close();
    return true;
  }

  char name[128];
  char path[160];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (isDirectory) entry.getName(name, sizeof(name));
    entry.close();
    if (!isDirectory || name[0] == '.' || name[0] == '_') continue;

    bool duplicate = false;
    for (const auto& family : families) {
      if (family.name == name) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    const int length = std::snprintf(path, sizeof(path), "%s/%s", rootPath, name);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) continue;
    SdCardFontFamilyInfo family;
    family.name = name;
    if (!scanFamilySummary(path, family)) {
      root.close();
      return false;
    }
    // Invalid/empty directories do not consume the catalog limit and cannot
    // shadow a valid family in the lower-priority visible root.
    if (family.indexCount > 0) {
      family.sourceVisibleRoot = visibleRoot;
      if (families.size() < SdCardFontRegistry::MAX_SD_FAMILIES) {
        families.push_back(std::move(family));
      } else {
        // Preserve the prior stable alphabetical cap without retaining every
        // family: replace the current largest name when a smaller one appears.
        auto largest = std::max_element(
            families.begin(), families.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });
        if (family.name < largest->name) *largest = std::move(family);
      }
    }
  }
  const bool ok = !FsHelpers::directoryIterationFailed(root);
  root.close();
  return ok;
}

bool writeFamilyDetails(HalFile& index, const char* dirPath, const uint16_t expectedCount,
                        fontcatalog::Entry& summary) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return false;
  }

  // This cold rebuild path uses bounded stack buffers instead of one heap
  // string per font. It runs before Wi-Fi when entering the web server.
  uint8_t seenSizes[32] = {};
  char name[128];
  char path[fontcatalog::MaxPath + 1];
  bool ok = true;
  while (ok) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(name, sizeof(name));
    entry.close();
    if (isDirectory || name[0] == '.' || name[0] == '_') continue;

    uint8_t size = 0, style = 0;
    if (!SdCardFontRegistry::parseFilename(name, size, style)) continue;
    const int length = std::snprintf(path, sizeof(path), "%s/%s", dirPath, name);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) continue;
    const uint8_t mask = uint8_t(1U << (size & 7U));
    if (seenSizes[size >> 3U] & mask) continue;
    seenSizes[size >> 3U] |= mask;

    const uint8_t meta[] = {size, style, uint8_t(length)};
    summary.hash = fontcatalog::hashBytes(summary.hash, meta, sizeof(meta));
    summary.hash = fontcatalog::hashBytes(summary.hash, path, length);
    ok = index.write(meta, sizeof(meta)) == sizeof(meta) &&
         index.write(path, static_cast<size_t>(length)) == static_cast<size_t>(length);
    summary.bytes += sizeof(meta) + static_cast<uint32_t>(length);
    ++summary.count;
  }
  ok = ok && !FsHelpers::directoryIterationFailed(dir) && summary.count == expectedCount;
  dir.close();
  return ok;
}
}  // namespace

bool SdCardFontRegistry::discover() {
  const uint32_t generation = indexGeneration.load(std::memory_order_acquire);
  discoveryFailed_ = false;
  uint64_t fingerprint = 0;
  if (!fontcatalog::inventory(fingerprint) || !rebuildIndex(fingerprint, generation)) {
    discoveryFailed_ = true;
    clear();
    LOG_ERR("SDREG", "Font index rebuild failed; retrying next request");
    return false;
  }
  inventoryFingerprint_ = fingerprint;
  inventoryGeneration_ = generation;
  inventoryKnown_ = true;
  ++revision_;
  if (!readIndex(fingerprint)) {
    discoveryFailed_ = true;
    clear();
    return false;
  }
  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

void SdCardFontRegistry::clear() { std::vector<SdCardFontFamilyInfo>().swap(families_); }

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!familyName || !*familyName) return nullptr;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  // If exactly one of the roots already exists, keep using it. Otherwise
  // (neither exists, or both exist) prefer the hidden root for new installs.
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

#if CROSSPOINT_VECTOR_FONTS
namespace {
constexpr size_t MAX_VECTOR_FAMILIES = 32;

// Match a vector font filename (.ttf/.otf/.ttc, case-insensitive); baseLen = length without extension.
bool parseVectorFontName(const char* filename, size_t& baseLen) {
  static constexpr const char* kExts[] = {".ttf", ".otf", ".ttc"};
  const size_t nameLen = strlen(filename);
  for (const char* ext : kExts) {
    const size_t extLen = strlen(ext);
    if (nameLen <= extLen) continue;
    if (strcasecmp(filename + nameLen - extLen, ext) == 0) {
      baseLen = nameLen - extLen;
      return baseLen > 0 && baseLen <= 127;
    }
  }
  return false;
}

// Style role (bit0 = bold, bit1 = italic) inferred from case-insensitive filename tokens.
uint8_t parseVectorStyle(const char* baseName, size_t baseLen) {
  bool bold = false;
  bool ital = false;
  for (size_t i = 0; i < baseLen; ++i) {
    if ((baseLen - i) >= 4 && strncasecmp(baseName + i, "bold", 4) == 0) bold = true;
    if ((baseLen - i) >= 6 && strncasecmp(baseName + i, "italic", 6) == 0) ital = true;
    if ((baseLen - i) >= 7 && strncasecmp(baseName + i, "oblique", 7) == 0) ital = true;
  }
  return static_cast<uint8_t>((bold ? 1 : 0) | (ital ? 2 : 0));
}

unsigned long inspectRead(void* ctx, const unsigned long offset, unsigned char* buffer, const unsigned long count) {
  auto* f = static_cast<HalFile*>(ctx);
  if (f == nullptr || !*f) return 0;
  if (!f->seek(static_cast<size_t>(offset))) return 0;
  if (count == 0) return 0;
  const int n = f->read(buffer, count);
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

// Refine each file's style role from the face's own metadata (OS/2 weight, italic flag), guarantee a
// regular anchor, then dedup by role (first file wins).
void refineVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files) {
  using freeink::font::FtFont;
  for (size_t i = 0; i < files.size();) {
    auto& info = files[i];
    HalFile f = Storage.open(info.path.c_str());
    if (!f || f.isDirectory()) {
      ++i;
      continue;
    }
    FtFont::FaceInfo face;
    const auto result = FtFont::inspectStream(&inspectRead, &f, static_cast<unsigned long>(f.size()), face);
    if (result == FtFont::InspectResult::Unsupported) {
      // The pinned FreeType build has only the TrueType driver: CFF/PostScript-outline .otf files
      // cannot be opened. Drop the file so the family is not listed (or that style is synthesized from
      // the regular face) instead of silently falling back to the built-in font when selected.
      LOG_ERR("SDREG", "Skipping unsupported font file (CFF/PostScript outlines?): %s", info.path.c_str());
      files.erase(files.begin() + i);
      continue;
    }
    if (result == FtFont::InspectResult::Ok) {
      info.style = static_cast<uint8_t>((face.weight >= 600 ? 1 : 0) | (face.italic ? 2 : 0));
    }  // unreadable: keep the filename-derived role
    ++i;
  }
  if (files.empty()) return;
  bool haveRegular = false;
  for (const auto& info : files) haveRegular = haveRegular || info.style == 0;
  if (!haveRegular && !files.empty()) {
    SdCardFontFileInfo* pick = &files.front();
    for (auto& info : files) {
      if ((info.style & 2) == 0) {
        pick = &info;
        break;
      }
    }
    LOG_DBG("SDREG", "No regular face in %s - promoting %s", dirPath, pick->path.c_str());
    pick->style = 0;
  }
  for (size_t i = 0; i < files.size(); ++i) {
    for (size_t j = i + 1; j < files.size();) {
      if (files[j].style == files[i].style) {
        LOG_ERR("SDREG", "Duplicate style role in %s (%s) - skipping", dirPath, files[j].path.c_str());
        files.erase(files.begin() + j);
      } else {
        ++j;
      }
    }
  }
}
}  // namespace

void SdCardFontRegistry::appendVectorFamilies() {
  // -fno-exceptions: a failed vector growth aborts, so check headroom and reserve once, up front.
  const auto heap = MemoryBudget::snapshot();
  if (families_.size() >= static_cast<size_t>(MAX_SD_FAMILIES) || heap.freeHeap < 24576 ||
      heap.maxAllocHeap < 12288) {
    LOG_ERR("SDREG", "Skipping TTF/OTF discovery (free=%u maxAlloc=%u)", static_cast<unsigned>(heap.freeHeap),
            static_cast<unsigned>(heap.maxAllocHeap));
    return;
  }
  const size_t room = std::min<size_t>(MAX_VECTOR_FAMILIES, MAX_SD_FAMILIES - families_.size());
  families_.reserve(families_.size() + room);
  size_t added = 0;

  for (const char* root : {FONTS_DIR_HIDDEN, FONTS_DIR_VISIBLE}) {
    char resolved[16];
    const char* rootPath = FsHelpers::resolveRootDirectoryIgnoreCase(root, resolved, sizeof(resolved)) ? resolved : root;
    HalFile dir = Storage.open(rootPath);
    if (!dir || !dir.isDirectory()) {
      dir.close();
      continue;
    }
    char name[128];
    char path[160];
    while (added < room) {
      HalFile entry = dir.openNextFile();
      if (!entry) break;
      const bool isDirectory = entry.isDirectory();
      entry.getName(name, sizeof(name));
      entry.close();
      if (name[0] == '.' || name[0] == '_') continue;

      if (!isDirectory) {
        // Loose file directly under the root: one family named after the file.
        size_t baseLen = 0;
        if (!parseVectorFontName(name, baseLen)) continue;
        std::string familyName(name, baseLen);
        if (findSummary(familyName)) continue;  // existing family (cpfont or earlier vector) wins
        SdCardFontFileInfo info;
        info.path = std::string(rootPath) + "/" + name;
        info.pointSize = 0;
        info.style = 0;
        {
          std::vector<SdCardFontFileInfo> probe;
          probe.push_back(info);
          refineVectorStyles(rootPath, probe);  // drops it when the engine cannot open the file
          if (probe.empty()) continue;
        }
        SdCardFontFamilyInfo family;
        family.name = std::move(familyName);
        family.vector = true;
        family.files.push_back(std::move(info));
        families_.push_back(std::move(family));
        ++added;
        continue;
      }

      // Family folder holding .ttf/.otf/.ttc files (a cpfont family of the same name wins).
      if (findSummary(std::string(name))) continue;
      const int length = std::snprintf(path, sizeof(path), "%s/%s", rootPath, name);
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) continue;
      HalFile sub = Storage.open(path);
      if (!sub || !sub.isDirectory()) {
        sub.close();
        continue;
      }
      std::vector<SdCardFontFileInfo> files;
      char fileName[128];
      while (files.size() < 8) {
        HalFile f = sub.openNextFile();
        if (!f) break;
        const bool fileIsDir = f.isDirectory();
        f.getName(fileName, sizeof(fileName));
        f.close();
        size_t baseLen = 0;
        if (fileIsDir || fileName[0] == '.' || fileName[0] == '_' || !parseVectorFontName(fileName, baseLen)) continue;
        SdCardFontFileInfo info;
        info.path = std::string(path) + "/" + fileName;
        info.pointSize = 0;
        info.style = parseVectorStyle(fileName, baseLen);
        files.push_back(std::move(info));
      }
      sub.close();
      if (files.empty()) continue;
      refineVectorStyles(path, files);
      if (files.empty()) continue;  // every file was unsupported (e.g. all CFF .otf): do not list the family
      SdCardFontFamilyInfo family;
      family.name = name;
      family.vector = true;
      family.files = std::move(files);
      families_.push_back(std::move(family));
      ++added;
    }
    dir.close();
  }
  if (added > 0) {
    std::sort(families_.begin(), families_.end(),
              [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });
    LOG_DBG("SDREG", "Found %u TTF/OTF families", static_cast<unsigned>(added));
  }
}
#endif  // CROSSPOINT_VECTOR_FONTS

const SdCardFontFamilyInfo* SdCardFontRegistry::findSummary(const std::string& name) const {
  for (const auto& family : families_)
    if (family.name == name) return &family;
  return nullptr;
}
const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  const auto* family = findSummary(name);
  return family && family->ensureDetails() ? family : nullptr;
}

namespace fontcatalog {
namespace {
bool inventoryDirectory(const char* path, uint64_t& hash, bool children) {
  HalFile dir = Storage.open(path);
  if (!dir) {
    const bool ok = !dir.allocationFailed();
    dir.close();
    return ok;
  }
  if (!dir.isDirectory()) {
    dir.close();
    return true;
  }
  char name[128];
  char child[160];
  bool ok = true;
  while (true) {
    HalFile file = dir.openNextFile();
    if (!file) break;
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    const uint64_t size = isDir ? 0 : file.fileSize64();
    file.close();
    if (name[0] == '.' || name[0] == '_') continue;
    // FNV-1a over inventory only. No TTF parsing or glyph/font-data reads.
    for (const char* p = name; *p; ++p) hash = (hash ^ uint8_t(*p)) * 1099511628211ull;
    hash = (hash ^ uint8_t(isDir)) * 1099511628211ull;
    hash = (hash ^ size) * 1099511628211ull;
    if (isDir && children) {
      const int length = std::snprintf(child, sizeof(child), "%s/%s", path, name);
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(child) || !inventoryDirectory(child, hash, false)) {
        ok = false;
        break;
      }
    }
  }
  ok = ok && !FsHelpers::directoryIterationFailed(dir);
  dir.close();
  return ok;
}
}  // namespace
bool inventory(uint64_t& fingerprint) {
  fingerprint = 14695981039346656037ull;
  for (const char* root : {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE}) {
    char resolved[16];
    const char* path = FsHelpers::resolveRootDirectoryIgnoreCase(root, resolved, sizeof(resolved)) ? resolved : root;
    if (!inventoryDirectory(path, fingerprint, true)) {
      LOG_ERR("SDREG", "Could not validate font inventory");
      return false;
    }
    fingerprint = (fingerprint ^ 0xff) * 1099511628211ull;
  }
  return true;
}
}  // namespace fontcatalog

bool SdCardFontRegistry::needsRefresh() const {
  return discoveryFailed_ || !inventoryKnown_ ||
         inventoryGeneration_ != indexGeneration.load(std::memory_order_acquire);
}

void SdCardFontRegistry::invalidateIndex() {
  indexGeneration.fetch_add(1, std::memory_order_acq_rel);
  if (Storage.exists(fontcatalog::Path) && !Storage.remove(fontcatalog::Path))
    LOG_ERR("SDREG", "Could not invalidate font index");
}

bool SdCardFontRegistry::loadNames(bool checkInventory) {
  const uint32_t generation = indexGeneration.load(std::memory_order_acquire);
  if (checkInventory || !inventoryKnown_ || inventoryGeneration_ != generation) {
    if (!fontcatalog::inventory(inventoryFingerprint_)) {
      discoveryFailed_ = true;
      return false;
    }
    inventoryGeneration_ = generation;
    inventoryKnown_ = true;
  }
  const uint64_t fingerprint = inventoryFingerprint_;
  discoveryFailed_ = false;
  if (readIndex(fingerprint)) return !families_.empty();
  if (discoveryFailed_) return false;
  LOG_DBG("SDREG", "Font index missing or stale; rebuilding");
  return discover();
}

bool SdCardFontRegistry::readIndex(uint64_t fingerprint) {
  using namespace fontcatalog;
  HalFile file = Storage.open(Path);
  if (!file) {
    file.close();
    return false;
  }
  Header header;
  const size_t length = file.size();
  bool ok = length <= MaxBytes && file.read(&header, sizeof(header)) == sizeof(header) && header.magic == Magic &&
            header.version == Version && header.inventory == fingerprint && header.reserved == 0 &&
            header.count <= MAX_SD_FAMILIES && length >= sizeof(Header) + header.count * sizeof(Entry);
  if (!ok) {
    file.close();
    return false;
  }
  clear();
  // Names only: at most 128 bounded names and empty vectors, never font files.
  const auto heap = MemoryBudget::snapshot();
  const size_t arrayBytes = header.count * sizeof(SdCardFontFamilyInfo);
  const size_t required = arrayBytes + header.count * 144U + 8192U;
  if (heap.freeHeap < required || heap.maxAllocHeap < std::max(arrayBytes, size_t(128))) {
    file.close();
    discoveryFailed_ = true;
    LOG_ERR("SDREG", "Insufficient memory for font names");
    return false;
  }
  families_.reserve(header.count);
  uint32_t next = sizeof(Header) + header.count * sizeof(Entry);
  for (uint32_t i = 0; ok && i < header.count; ++i) {
    Entry entry;
    ok = file.read(&entry, sizeof(entry)) == sizeof(entry) && std::memchr(entry.name, '\0', sizeof(entry.name)) &&
         entry.checksum == hashBytes(2166136261u, &entry, offsetof(Entry, checksum)) && entry.name[0] &&
         entry.count > 0 && entry.count <= MaxFiles && entry.offset == next && entry.bytes <= length - next &&
         entry.first > 0 && entry.last >= entry.first;
    if (!ok) break;
    if (!families_.empty() && families_.back().name >= entry.name) {
      ok = false;
      break;
    }
    SdCardFontFamilyInfo family;
    family.name = entry.name;
    family.firstSize = entry.first;
    family.lastSize = entry.last;
    family.indexOffset = entry.offset;
    family.indexBytes = entry.bytes;
    family.indexHash = entry.hash;
    family.indexCount = entry.count;
    next += entry.bytes;
    families_.push_back(std::move(family));
  }
  file.close();
  if (!ok || next != length) {
    clear();
    return false;
  }
  discoveryFailed_ = false;
  LOG_DBG("SDREG", "Font index: loaded %u names, no family paths", unsigned(header.count));
#if CROSSPOINT_VECTOR_FONTS
  appendVectorFamilies();
#endif
  return true;
}

bool SdCardFontRegistry::rebuildIndex(uint64_t fingerprint, uint32_t generation) {
  using namespace fontcatalog;
  clear();
  const auto heap = MemoryBudget::snapshot();
  const size_t arrayBytes = MAX_SD_FAMILIES * sizeof(SdCardFontFamilyInfo);
  const size_t required = arrayBytes + MAX_SD_FAMILIES * 144U + 8192U;
  if (heap.freeHeap < required || heap.maxAllocHeap < arrayBytes) {
    LOG_ERR("SDREG", "Insufficient memory to rebuild font index");
    return false;
  }
  families_.reserve(MAX_SD_FAMILIES);

  char hiddenRoot[16];
  char visibleRoot[16];
  const char* hiddenPath = FsHelpers::resolveRootDirectoryIgnoreCase(FONTS_DIR_HIDDEN, hiddenRoot, sizeof(hiddenRoot))
                               ? hiddenRoot
                               : FONTS_DIR_HIDDEN;
  const char* visiblePath =
      FsHelpers::resolveRootDirectoryIgnoreCase(FONTS_DIR_VISIBLE, visibleRoot, sizeof(visibleRoot))
          ? visibleRoot
          : FONTS_DIR_VISIBLE;
  if (!addFamilySummaries(hiddenPath, false, families_) || !addFamilySummaries(visiblePath, true, families_))
    return false;
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });
  char familyPath[160];

  if (!Storage.mkdir("/.crosspoint", true) && !Storage.exists("/.crosspoint")) return false;
  HalFile file;
  if (!Storage.openFileForWrite("SDREG", TempPath, file)) return false;
  Header header;
  header.inventory = fingerprint;
  header.count = families_.size();
  bool ok = file.write(&header, sizeof(header)) == sizeof(header);
  // SdFat cannot seek past EOF. Materialize the summary table before writing
  // detail blocks, then seek back only within the existing file to fill entries.
  for (uint32_t i = 0; ok && i < header.count; ++i) {
    const Entry blank;
    ok = file.write(&blank, sizeof(blank)) == sizeof(blank);
  }
  uint32_t offset = sizeof(Header) + header.count * sizeof(Entry);
  for (size_t i = 0; ok && i < families_.size(); ++i) {
    const auto& family = families_[i];
    Entry entry;
    const char* familyRoot = family.sourceVisibleRoot ? visiblePath : hiddenPath;
    const int pathLength = std::snprintf(familyPath, sizeof(familyPath), "%s/%s", familyRoot, family.name.c_str());
    if (family.name.size() >= sizeof(entry.name) || family.indexCount == 0 || family.indexCount > MaxFiles ||
        pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(familyPath)) {
      ok = false;
      break;
    }
    std::memcpy(entry.name, family.name.c_str(), family.name.size() + 1);
    entry.offset = offset;
    entry.first = family.firstSize;
    entry.last = family.lastSize;
    entry.hash = 2166136261u;
    ok = file.seek(offset);
    if (ok) ok = writeFamilyDetails(file, familyPath, family.indexCount, entry);
    entry.checksum = hashBytes(2166136261u, &entry, offsetof(Entry, checksum));
    offset += entry.bytes;
    ok = ok && offset <= MaxBytes && file.seek(sizeof(Header) + i * sizeof(Entry)) &&
         file.write(&entry, sizeof(entry)) == sizeof(entry);
  }
  ok = file.sync() && ok && generation == indexGeneration.load(std::memory_order_acquire);
  file.close();
  if (ok) {
    if (Storage.exists(Path)) ok = Storage.remove(Path);
    if (ok) ok = Storage.rename(TempPath, Path);
    if (ok && generation != indexGeneration.load(std::memory_order_acquire)) {
      Storage.remove(Path);
      ok = false;
    }
  }
  if (!ok) {
    Storage.remove(TempPath);
    LOG_ERR("SDREG", "Could not save font index; using discovered catalog");
  }
  return ok;
}

bool SdCardFontFamilyInfo::ensureDetails() const {
  if (!files.empty() || indexOffset == 0) return true;
  using namespace fontcatalog;
  HalFile file = Storage.open(Path);
  bool ok = file && indexCount > 0 && indexCount <= MaxFiles && indexBytes <= MaxBytes && indexOffset <= file.size() &&
            indexBytes <= file.size() - indexOffset && file.seek(indexOffset);
  uint32_t remaining = indexBytes, hash = 2166136261u;
  // Only the chosen family is resident; paths are bounded to 255 bytes each.
  if (ok) {
    const auto heap = MemoryBudget::snapshot();
    const size_t arrayBytes = indexCount * sizeof(SdCardFontFileInfo);
    if (heap.freeHeap < arrayBytes + indexBytes + indexCount * 24U + 8192U ||
        heap.maxAllocHeap < arrayBytes + MaxPath) {
      file.close();
      LOG_ERR("SDREG", "Insufficient memory for font details: %s", name.c_str());
      return false;
    }
    files.reserve(indexCount);
  }
  for (uint16_t i = 0; ok && i < indexCount; ++i) {
    uint8_t meta[3];
    ok = remaining >= sizeof(meta) && file.read(meta, sizeof(meta)) == sizeof(meta);
    if (!ok) break;
    remaining -= sizeof(meta);
    ok = meta[0] != 0 && meta[2] > 0 && meta[2] <= remaining && meta[1] < 4;
    if (!ok) break;
    SdCardFontFileInfo font;
    font.pointSize = meta[0];
    font.style = meta[1];
    font.path.resize(meta[2]);
    ok = file.read(font.path.data(), meta[2]) == meta[2];
    if (!ok) break;
    hash = hashBytes(hash, meta, sizeof(meta));
    hash = hashBytes(hash, font.path.data(), font.path.size());
    remaining -= meta[2];
    ok = font.path.find('\0') == std::string::npos && font.path.find("/../") == std::string::npos &&
         (strncasecmp(font.path.c_str(), "/.fonts/", 8) == 0 || strncasecmp(font.path.c_str(), "/fonts/", 7) == 0);
    if (ok) files.push_back(std::move(font));
  }
  file.close();
  if (!ok || remaining != 0 || hash != indexHash) {
    std::vector<SdCardFontFileInfo>().swap(files);
    SdCardFontRegistry::invalidateIndex();
    LOG_ERR("SDREG", "Invalid font index details: %s; retry will rebuild", name.c_str());
    return false;
  }
  LOG_DBG("SDREG", "Font index: loaded %u files for %s", unsigned(files.size()), name.c_str());
  return true;
}

void SdCardFontFamilyInfo::releaseDetails() const {
  // Discovered families have no index block to reload from, so only release
  // details owned by an indexed summary.
  if (indexOffset != 0) std::vector<SdCardFontFileInfo>().swap(files);
}
