#pragma once

#include <HalStorage.h>

#include <array>
#include <cstddef>
#include <cstdint>

// The on-disk /reader_settings.bin format: a versioned per-book override of a
// subset of global reader settings. Extracted out of EpubReaderActivity.cpp so
// the version-gated parsing -- the part most likely to break silently the next
// time a field is added -- is host-testable without pulling in the whole
// reader activity and its ~90 transitive includes. EpubReaderActivity.cpp's
// own normalizeRenderMode/normalizeRenderModeRaw/clampAutoPageTurnIntervalSeconds
// are deliberately NOT moved here (they have ~25 unrelated call sites); this
// header's .cpp keeps small mirrors of the two of them it actually needs.
namespace ReaderSettingsIO {

struct ReaderSettingsSnapshot {
  uint8_t fontFamily = 0;
  uint8_t readerFontPointSize = 14;
  uint8_t lineHeightPercent = 100;
  uint8_t wordSpacing = 0;
  // Mirrors CrossPointSettings::characterSpacing's own default (5) -- kept as a literal so this
  // header doesn't need CrossPointSettings.h (and its ArduinoJson dependency) just for one
  // constant used only as a default-member-initializer. 0..10 represents -5..+5.
  uint8_t characterSpacing = 5;
  uint8_t orientation = 0;
  uint8_t screenMarginVertical = 5;
  uint8_t screenMarginHorizontal = 5;
  uint8_t publisherPageNumbers = 0;
  uint8_t paragraphAlignment = 0;
  uint8_t embeddedStyle = 1;
  uint8_t hyphenationEnabled = 0;
  uint8_t textAntiAliasing = 1;
  uint8_t imageRendering = 0;
  uint8_t imageGrayscale = 1;
  uint8_t extraParagraphSpacing = 1;
  uint8_t forceParagraphIndents = 0;
  uint8_t focusReadingEnabled = 0;
  uint8_t guideReadingEnabled = 0;
  uint8_t epubRenderMode = 0;
  // Mirrors CrossPointSettings::INDEXING_FULL_SECTION (1); see characterSpacing above.
  uint8_t indexingMethod = 1;
  char sdFontFamilyName[64] = "";
};

struct BookReaderSettingsData {
  bool hasAutoPageTurnInterval = false;
  uint16_t autoPageTurnSeconds = 0;
  bool hasCustomReaderSettings = false;
  uint32_t readerSettingsOverrideMask = 0;
  bool hasSafeModeOverride = false;
  bool hasRenderModeOverride = false;
  bool hasDictionaryFontOverride = false;
  uint8_t renderMode = 0;
  char dictionarySdFontFamilyName[64] = "";
  uint8_t dictionaryFontPointSize = 0;
  ReaderSettingsSnapshot readerSettings;
};

// File format versions. v10 (upstream): per-field override mask. v11 (ours): appends the
// Character Spacing byte after the mask. v12 (upstream, folded in on top of ours): appends an
// Image Grayscale byte after that. Each new field is append-only at the tail -- never reorder or
// reuse a version number for a different meaning, or an old on-disk file gets misread.
constexpr uint8_t LEGACY_READER_SETTINGS_FILE_VERSION = 1;
constexpr uint8_t PRE_WORD_SPACING_READER_SETTINGS_FILE_VERSION = 2;
constexpr uint8_t PRE_INDEXING_METHOD_READER_SETTINGS_FILE_VERSION = 3;
constexpr uint8_t PRE_DICTIONARY_FONT_READER_SETTINGS_FILE_VERSION = 4;
constexpr uint8_t PRE_POINT_SIZE_READER_SETTINGS_FILE_VERSION = 5;
constexpr uint8_t PRE_DICTIONARY_FONT_SIZE_READER_SETTINGS_FILE_VERSION = 6;
constexpr uint8_t PRE_SPLIT_SCREEN_MARGIN_READER_SETTINGS_FILE_VERSION = 7;
constexpr uint8_t PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION = 8;
constexpr uint8_t PRE_FIELD_OVERRIDES_READER_SETTINGS_FILE_VERSION = 9;
constexpr uint8_t PRE_CHARACTER_SPACING_READER_SETTINGS_FILE_VERSION = 10;
// Mask layout before Image Grayscale existed: bit 18 was the SD font override, bit 19 (from
// v11 on) was Character Spacing. From v12 on, Image Grayscale takes bit 18 and the other two
// each shift up by one -- see the remap in readReaderSettingsSnapshot's caller.
constexpr uint8_t PRE_IMAGE_GRAYSCALE_READER_SETTINGS_FILE_VERSION = 11;
constexpr uint8_t READER_SETTINGS_FILE_VERSION = 12;

constexpr uint8_t READER_SETTINGS_FLAG_CUSTOM = 1 << 0;
constexpr uint8_t READER_SETTINGS_FLAG_AUTO_PAGE_TURN = 1 << 1;
constexpr uint8_t READER_SETTINGS_FLAG_RENDER_MODE = 1 << 2;
constexpr uint8_t READER_SETTINGS_FLAG_DICTIONARY_FONT = 1 << 3;
constexpr uint8_t READER_SETTINGS_FLAG_SAFE_MODE = 1 << 4;

constexpr char READER_SETTINGS_FILE_NAME[] = "/reader_settings.bin";

constexpr std::array<uint8_t ReaderSettingsSnapshot::*, 19> READER_SETTING_FIELDS = {
    &ReaderSettingsSnapshot::fontFamily,
    &ReaderSettingsSnapshot::readerFontPointSize,
    &ReaderSettingsSnapshot::lineHeightPercent,
    &ReaderSettingsSnapshot::wordSpacing,
    &ReaderSettingsSnapshot::orientation,
    &ReaderSettingsSnapshot::screenMarginVertical,
    &ReaderSettingsSnapshot::screenMarginHorizontal,
    &ReaderSettingsSnapshot::publisherPageNumbers,
    &ReaderSettingsSnapshot::paragraphAlignment,
    &ReaderSettingsSnapshot::embeddedStyle,
    &ReaderSettingsSnapshot::hyphenationEnabled,
    &ReaderSettingsSnapshot::textAntiAliasing,
    &ReaderSettingsSnapshot::imageRendering,
    &ReaderSettingsSnapshot::extraParagraphSpacing,
    &ReaderSettingsSnapshot::forceParagraphIndents,
    &ReaderSettingsSnapshot::focusReadingEnabled,
    &ReaderSettingsSnapshot::guideReadingEnabled,
    &ReaderSettingsSnapshot::indexingMethod,
    &ReaderSettingsSnapshot::imageGrayscale,
};
// Image Grayscale joined the table last (bit 18), which is why SD_FONT_FAMILY_OVERRIDE and
// CHARACTER_SPACING_OVERRIDE below shift up by one automatically from their pre-v12 bits
// (18, 19) to (19, 20) -- see the v11-and-earlier remap in ReaderSettingsSnapshotIO.cpp.
constexpr uint32_t IMAGE_GRAYSCALE_OVERRIDE = 1U << (READER_SETTING_FIELDS.size() - 1);
constexpr uint32_t SD_FONT_FAMILY_OVERRIDE = 1U << READER_SETTING_FIELDS.size();
// Character Spacing is ours, so it gets its own bit after upstream's fields instead of joining
// the table (which would shift SD_FONT_FAMILY_OVERRIDE's bit inside already-written v10 files).
constexpr uint32_t CHARACTER_SPACING_OVERRIDE = SD_FONT_FAMILY_OVERRIDE << 1;
constexpr uint32_t ALL_READER_SETTING_OVERRIDES = (CHARACTER_SPACING_OVERRIDE << 1) - 1;
constexpr uint32_t READER_FONT_OVERRIDES = (1U << 0) | (1U << 1) | SD_FONT_FAMILY_OVERRIDE;
// Anti-aliasing and grayscale change page drawing, but not the saved line/page layout.
constexpr uint32_t READER_LAYOUT_SETTING_OVERRIDES =
    ALL_READER_SETTING_OVERRIDES & ~((1U << 11) | IMAGE_GRAYSCALE_OVERRIDE);
constexpr uint32_t SAFE_MODE_SETTING_OVERRIDES = (1U << 9) | (1U << 15) | (1U << 16);

uint32_t changedReaderSettingsMask(const ReaderSettingsSnapshot& current, const ReaderSettingsSnapshot& global);
void applyReaderSettingsOverrides(ReaderSettingsSnapshot& target, const ReaderSettingsSnapshot& book, uint32_t mask);

bool readReaderSettingsSnapshot(FsFile& file, ReaderSettingsSnapshot& out, bool includesWordSpacing,
                                bool includesIndexingMethod, bool includesSplitScreenMargins,
                                bool includesLegacyReaderDarkMode);
bool writeReaderSettingsSnapshot(FsFile& file, const ReaderSettingsSnapshot& in);

// Mirrors EpubReaderActivity::BookSettingsReadStatus (a type alias of this), reported for a file
// already known to exist -- the caller decides "Missing" itself before ever opening the file.
enum class ReadStatus : uint8_t { Missing, Loaded, Invalid };

// Parses an already-open reader_settings.bin file positioned at its start. `defaults` supplies
// the current global reader settings -- used both as the pre-v10 fallback (whole snapshot) and
// as what non-overridden fields inherit -- plus the two dictionary defaults, mirroring what
// EpubReaderActivity passes from the live SETTINGS singleton. Always closes `file`, on every
// return path, exactly once, matching the original inline implementation. `status`, if given, is
// set to Loaded on a fully successful parse and left at Invalid (never Missing -- the file is
// already open) on any version-mismatch or truncated-read failure.
BookReaderSettingsData parseBookReaderSettingsFile(FsFile& file, const ReaderSettingsSnapshot& defaults,
                                                    const char* defaultDictionarySdFontFamilyName,
                                                    uint8_t defaultDictionaryFontPointSize,
                                                    ReadStatus* status = nullptr);

// Writes the current version only -- this is never used to write a legacy version, only to read
// one back for migration. Always closes `file`.
bool writeBookReaderSettingsFile(FsFile& file, const BookReaderSettingsData& data);

}  // namespace ReaderSettingsIO
