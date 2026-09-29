#include "ReaderSettingsSnapshotIO.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "Epub/EpubRenderMode.h"

namespace ReaderSettingsIO {

namespace {

constexpr uint16_t MIN_AUTO_PAGE_TURN_INTERVAL_S = 5;
constexpr uint16_t MAX_AUTO_PAGE_TURN_INTERVAL_S = 120;
// Mirrors CrossPointSettings::{MIN,MAX}_SCREEN_MARGIN / MAX_WORD_SPACING / MAX_CHARACTER_SPACING /
// INDEXING_METHOD_COUNT / INDEXING_FULL_SECTION -- literals rather than including CrossPointSettings.h
// (and its ArduinoJson dependency) into this otherwise dependency-free module. See the header comment.
constexpr uint8_t MIN_SCREEN_MARGIN = 5;
constexpr uint8_t MAX_WORD_SPACING = 4;
constexpr uint8_t MAX_CHARACTER_SPACING = 4;
constexpr uint8_t INDEXING_FULL_SECTION = 1;
constexpr uint8_t INDEXING_METHOD_COUNT = 2;

// Mirrors EpubReaderActivity.cpp's own normalizeRenderMode/normalizeRenderModeRaw -- not shared
// via a common call site because those have ~25 unrelated call sites throughout that file; these
// are small, stable, and unlikely to drift (see the header's top comment).
EpubRenderMode normalizeRenderMode(const uint8_t rawMode) {
  return isValidEpubRenderMode(rawMode) ? static_cast<EpubRenderMode>(rawMode) : EpubRenderMode::CrossInkDefault;
}
uint8_t normalizeRenderModeRaw(const uint8_t rawMode) { return static_cast<uint8_t>(normalizeRenderMode(rawMode)); }

uint16_t clampAutoPageTurnIntervalSeconds(const uint16_t seconds) {
  return std::clamp(seconds, MIN_AUTO_PAGE_TURN_INTERVAL_S, MAX_AUTO_PAGE_TURN_INTERVAL_S);
}

bool readExact(FsFile& file, void* data, const size_t size) { return file.read(data, size) == static_cast<int>(size); }
bool writeExact(FsFile& file, const void* data, const size_t size) { return file.write(data, size) == size; }

bool readU8(FsFile& file, uint8_t& value) { return readExact(file, &value, sizeof(value)); }
bool writeU8(FsFile& file, const uint8_t value) { return writeExact(file, &value, sizeof(value)); }

bool readU16(FsFile& file, uint16_t& value) {
  uint8_t data[2] = {};
  if (!readExact(file, data, sizeof(data))) return false;
  value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}
bool writeU16(FsFile& file, const uint16_t value) {
  const uint8_t data[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
  return writeExact(file, data, sizeof(data));
}

bool readU32(FsFile& file, uint32_t& value) {
  uint8_t data[4] = {};
  if (!readExact(file, data, sizeof(data))) return false;
  value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
          (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}
bool writeU32(FsFile& file, const uint32_t value) {
  const uint8_t data[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                           static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return writeExact(file, data, sizeof(data));
}

}  // namespace

uint32_t changedReaderSettingsMask(const ReaderSettingsSnapshot& current, const ReaderSettingsSnapshot& global) {
  uint32_t mask = 0;
  for (size_t i = 0; i < READER_SETTING_FIELDS.size(); ++i) {
    const auto field = READER_SETTING_FIELDS[i];
    if (current.*field != global.*field) mask |= 1U << i;
  }
  if (std::strcmp(current.sdFontFamilyName, global.sdFontFamilyName) != 0) mask |= SD_FONT_FAMILY_OVERRIDE;
  if (current.characterSpacing != global.characterSpacing) mask |= CHARACTER_SPACING_OVERRIDE;
  return mask;
}

void applyReaderSettingsOverrides(ReaderSettingsSnapshot& target, const ReaderSettingsSnapshot& book,
                                  const uint32_t mask) {
  for (size_t i = 0; i < READER_SETTING_FIELDS.size(); ++i) {
    if (mask & (1U << i)) {
      const auto field = READER_SETTING_FIELDS[i];
      target.*field = book.*field;
    }
  }
  if (mask & SD_FONT_FAMILY_OVERRIDE) {
    std::memcpy(target.sdFontFamilyName, book.sdFontFamilyName, sizeof(target.sdFontFamilyName));
  }
  if (mask & CHARACTER_SPACING_OVERRIDE) target.characterSpacing = book.characterSpacing;
}

bool readReaderSettingsSnapshot(FsFile& file, ReaderSettingsSnapshot& out, const bool includesWordSpacing,
                                const bool includesIndexingMethod, const bool includesSplitScreenMargins,
                                const bool includesLegacyReaderDarkMode) {
  if (!(readU8(file, out.fontFamily) && readU8(file, out.readerFontPointSize) && readU8(file, out.lineHeightPercent) &&
        (!includesWordSpacing || readU8(file, out.wordSpacing)) && readU8(file, out.orientation))) {
    return false;
  }

  uint8_t legacyScreenMargin = MIN_SCREEN_MARGIN;
  const bool marginsRead = includesSplitScreenMargins
                               ? readU8(file, out.screenMarginVertical) && readU8(file, out.screenMarginHorizontal)
                               : readU8(file, legacyScreenMargin);
  if (!includesSplitScreenMargins) {
    out.screenMarginVertical = legacyScreenMargin;
    out.screenMarginHorizontal = legacyScreenMargin;
  }
  // Versions 2-8 wrote a per-book Dark Mode byte. Consume it to preserve
  // their remaining reader settings, but never let one book change the
  // global display inversion setting.
  uint8_t discardedLegacyReaderDarkMode = 0;
  if (!(marginsRead && readU8(file, out.publisherPageNumbers) && readU8(file, out.paragraphAlignment) &&
        readU8(file, out.embeddedStyle) && readU8(file, out.hyphenationEnabled) && readU8(file, out.textAntiAliasing) &&
        (!includesLegacyReaderDarkMode || readU8(file, discardedLegacyReaderDarkMode)) &&
        readU8(file, out.imageRendering) && readU8(file, out.extraParagraphSpacing) &&
        readU8(file, out.forceParagraphIndents) && readU8(file, out.focusReadingEnabled) &&
        readU8(file, out.guideReadingEnabled))) {
    return false;
  }
  if (!readU8(file, out.epubRenderMode)) {
    return false;
  }
  out.epubRenderMode = normalizeRenderModeRaw(out.epubRenderMode);
  if (includesIndexingMethod && !readU8(file, out.indexingMethod)) {
    return false;
  }
  return readExact(file, out.sdFontFamilyName, sizeof(out.sdFontFamilyName));
}

bool writeReaderSettingsSnapshot(FsFile& file, const ReaderSettingsSnapshot& in) {
  return writeU8(file, in.fontFamily) && writeU8(file, in.readerFontPointSize) && writeU8(file, in.lineHeightPercent) &&
         writeU8(file, std::min<uint8_t>(in.wordSpacing, MAX_WORD_SPACING)) &&
         writeU8(file, in.orientation) && writeU8(file, in.screenMarginVertical) &&
         writeU8(file, in.screenMarginHorizontal) && writeU8(file, in.publisherPageNumbers) &&
         writeU8(file, in.paragraphAlignment) && writeU8(file, in.embeddedStyle) &&
         writeU8(file, in.hyphenationEnabled) && writeU8(file, in.textAntiAliasing) &&
         writeU8(file, in.imageRendering) && writeU8(file, in.extraParagraphSpacing) &&
         writeU8(file, in.forceParagraphIndents) && writeU8(file, in.focusReadingEnabled) &&
         writeU8(file, in.guideReadingEnabled) && writeU8(file, normalizeRenderModeRaw(in.epubRenderMode)) &&
         writeU8(file, in.indexingMethod < INDEXING_METHOD_COUNT ? in.indexingMethod : INDEXING_FULL_SECTION) &&
         writeExact(file, in.sdFontFamilyName, sizeof(in.sdFontFamilyName));
}

BookReaderSettingsData parseBookReaderSettingsFile(FsFile& file, const ReaderSettingsSnapshot& defaults,
                                                    const char* defaultDictionarySdFontFamilyName,
                                                    const uint8_t defaultDictionaryFontPointSize) {
  BookReaderSettingsData data;
  data.readerSettings = defaults;
  std::strncpy(data.dictionarySdFontFamilyName, defaultDictionarySdFontFamilyName,
               sizeof(data.dictionarySdFontFamilyName) - 1);
  data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
  data.dictionaryFontPointSize = defaultDictionaryFontPointSize;

  uint8_t version = 0;
  if (!readU8(file, version)) {
    file.close();
    LOG_DBG("ERS", "Reader settings missing version, using defaults");
    return data;
  }

  if (version == LEGACY_READER_SETTINGS_FILE_VERSION) {
    uint16_t seconds = 0;
    if (readU16(file, seconds) && seconds != 0) {
      data.hasAutoPageTurnInterval = true;
      data.autoPageTurnSeconds = clampAutoPageTurnIntervalSeconds(seconds);
    }
    file.close();
    return data;
  }

  if (version != PRE_WORD_SPACING_READER_SETTINGS_FILE_VERSION &&
      version != PRE_INDEXING_METHOD_READER_SETTINGS_FILE_VERSION &&
      version != PRE_DICTIONARY_FONT_READER_SETTINGS_FILE_VERSION &&
      version != PRE_POINT_SIZE_READER_SETTINGS_FILE_VERSION &&
      version != PRE_DICTIONARY_FONT_SIZE_READER_SETTINGS_FILE_VERSION &&
      version != PRE_SPLIT_SCREEN_MARGIN_READER_SETTINGS_FILE_VERSION &&
      version != PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION &&
      version != PRE_FIELD_OVERRIDES_READER_SETTINGS_FILE_VERSION &&
      version != PRE_CHARACTER_SPACING_READER_SETTINGS_FILE_VERSION && version != READER_SETTINGS_FILE_VERSION) {
    file.close();
    LOG_DBG("ERS", "Reader settings version mismatch, using defaults");
    return data;
  }

  uint8_t flags = 0;
  uint16_t seconds = 0;
  uint8_t renderMode = static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  ReaderSettingsSnapshot snapshot;
  // Version 2 books inherit the current global indexing method instead of
  // silently changing modes when their older custom settings are loaded.
  snapshot.indexingMethod = data.readerSettings.indexingMethod;
  // Books saved before Character Spacing existed inherit the current global spacing.
  snapshot.characterSpacing = data.readerSettings.characterSpacing;
  bool ok = readU8(file, flags) && readU16(file, seconds);
  if (ok) {
    ok = readU8(file, renderMode);
  }
  if (ok) {
    ok = readReaderSettingsSnapshot(file, snapshot, version >= PRE_INDEXING_METHOD_READER_SETTINGS_FILE_VERSION,
                                    version >= PRE_DICTIONARY_FONT_READER_SETTINGS_FILE_VERSION,
                                    version >= PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION,
                                    version <= PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION);
  }
  if (ok && version >= PRE_POINT_SIZE_READER_SETTINGS_FILE_VERSION) {
    ok = readExact(file, data.dictionarySdFontFamilyName, sizeof(data.dictionarySdFontFamilyName));
  }
  if (ok && version >= PRE_SPLIT_SCREEN_MARGIN_READER_SETTINGS_FILE_VERSION) {
    ok = readU8(file, data.dictionaryFontPointSize);
  }
  uint32_t overrideMask = 0;
  if (ok && version >= PRE_CHARACTER_SPACING_READER_SETTINGS_FILE_VERSION) {
    ok = readU32(file, overrideMask) && (overrideMask & ~ALL_READER_SETTING_OVERRIDES) == 0;
  }
  if (ok && version >= READER_SETTINGS_FILE_VERSION) {
    ok = readU8(file, snapshot.characterSpacing);
  }
  file.close();
  if (!ok) {
    LOG_ERR("ERS", "Reader settings file is truncated, using defaults");
    return data;
  }

  if ((flags & READER_SETTINGS_FLAG_AUTO_PAGE_TURN) && seconds != 0) {
    data.hasAutoPageTurnInterval = true;
    data.autoPageTurnSeconds = clampAutoPageTurnIntervalSeconds(seconds);
  }
  if (flags & READER_SETTINGS_FLAG_CUSTOM) {
    // Older records owned the entire snapshot. New records only own the fields
    // the reader actually changed, so unrelated global defaults still apply.
    data.readerSettingsOverrideMask =
        version < PRE_CHARACTER_SPACING_READER_SETTINGS_FILE_VERSION ? ALL_READER_SETTING_OVERRIDES : overrideMask;
    // Files older than v11 never stored a spacing, so they must not pin one: the book follows the global value.
    if (version < READER_SETTINGS_FILE_VERSION) data.readerSettingsOverrideMask &= ~CHARACTER_SPACING_OVERRIDE;
    data.hasCustomReaderSettings = data.readerSettingsOverrideMask != 0;
    applyReaderSettingsOverrides(data.readerSettings, snapshot, data.readerSettingsOverrideMask);
  }
  data.hasSafeModeOverride = (flags & READER_SETTINGS_FLAG_SAFE_MODE) != 0;
  if (flags & READER_SETTINGS_FLAG_RENDER_MODE) {
    data.hasRenderModeOverride = true;
    data.renderMode = normalizeRenderModeRaw(renderMode);
  }
  if (flags & READER_SETTINGS_FLAG_DICTIONARY_FONT) {
    data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
    data.hasDictionaryFontOverride = data.dictionarySdFontFamilyName[0] != '\0';
  }
  if (!data.hasDictionaryFontOverride) {
    std::strncpy(data.dictionarySdFontFamilyName, defaultDictionarySdFontFamilyName,
                 sizeof(data.dictionarySdFontFamilyName) - 1);
    data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
    data.dictionaryFontPointSize = defaultDictionaryFontPointSize;
  }
  return data;
}

bool writeBookReaderSettingsFile(FsFile& file, const BookReaderSettingsData& data) {
  uint8_t flags = 0;
  if (data.readerSettingsOverrideMask != 0) flags |= READER_SETTINGS_FLAG_CUSTOM;
  if (data.hasSafeModeOverride) flags |= READER_SETTINGS_FLAG_SAFE_MODE;
  if (data.hasAutoPageTurnInterval) flags |= READER_SETTINGS_FLAG_AUTO_PAGE_TURN;
  if (data.hasRenderModeOverride) flags |= READER_SETTINGS_FLAG_RENDER_MODE;
  if (data.hasDictionaryFontOverride && data.dictionarySdFontFamilyName[0] != '\0') {
    flags |= READER_SETTINGS_FLAG_DICTIONARY_FONT;
  }
  const uint16_t clampedSeconds = clampAutoPageTurnIntervalSeconds(data.autoPageTurnSeconds);
  ReaderSettingsSnapshot normalizedReaderSettings = data.readerSettings;
  normalizedReaderSettings.epubRenderMode = normalizeRenderModeRaw(data.renderMode);
  const bool ok = writeU8(file, READER_SETTINGS_FILE_VERSION) && writeU8(file, flags) &&
                  writeU16(file, clampedSeconds) && writeU8(file, normalizeRenderModeRaw(data.renderMode)) &&
                  writeReaderSettingsSnapshot(file, normalizedReaderSettings) &&
                  writeExact(file, data.dictionarySdFontFamilyName, sizeof(data.dictionarySdFontFamilyName)) &&
                  writeU8(file, data.dictionaryFontPointSize) &&
                  writeU32(file, data.readerSettingsOverrideMask & ALL_READER_SETTING_OVERRIDES) &&
                  writeU8(file, std::min<uint8_t>(normalizedReaderSettings.characterSpacing, MAX_CHARACTER_SPACING));
  file.close();
  if (!ok) {
    LOG_ERR("ERS", "Short write saving reader settings");
  }
  return ok;
}

}  // namespace ReaderSettingsIO
