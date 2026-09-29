#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "Epub/EpubRenderMode.h"
#include "ReaderSettingsSnapshotIO.h"

using ReaderSettingsIO::ALL_READER_SETTING_OVERRIDES;
using ReaderSettingsIO::BookReaderSettingsData;
using ReaderSettingsIO::CHARACTER_SPACING_OVERRIDE;
using ReaderSettingsIO::ReaderSettingsSnapshot;

namespace {

// Independent little-endian byte packer for hand-building raw reader_settings.bin fixtures.
// Deliberately NOT implemented via ReaderSettingsIO::writeReaderSettingsSnapshot()/
// writeBookReaderSettingsFile(): using the code under test to build its own test input would
// let a matching bug on both sides cancel out. This mirrors the on-disk format purely from the
// documented byte layout (ReaderSettingsSnapshotIO.cpp's readReaderSettingsSnapshot/
// parseBookReaderSettingsFile), independently of the implementation being tested.
struct RawWriter {
  std::vector<uint8_t> bytes;
  void u8(const uint8_t v) { bytes.push_back(v); }
  void u16(const uint16_t v) {
    bytes.push_back(static_cast<uint8_t>(v & 0xFF));
    bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  }
  void u32(const uint32_t v) {
    bytes.push_back(static_cast<uint8_t>(v));
    bytes.push_back(static_cast<uint8_t>(v >> 8));
    bytes.push_back(static_cast<uint8_t>(v >> 16));
    bytes.push_back(static_cast<uint8_t>(v >> 24));
  }
  void fixed(const char* text, const size_t width) {
    const size_t len = std::strlen(text);
    for (size_t i = 0; i < width; ++i) bytes.push_back(i < len ? static_cast<uint8_t>(text[i]) : 0);
  }
};

// Appends one reader-settings snapshot body in the shape readReaderSettingsSnapshot() expects
// for a version-10-and-up file: word spacing included, indexing method included, split screen
// margins included, no legacy dark-mode byte. Field values are arbitrary but distinctive, so a
// mismatched byte offset shows up as a wrong-field assertion rather than an accidental pass.
void appendV10PlusSnapshotBody(RawWriter& w) {
  w.u8(3);    // fontFamily
  w.u8(16);   // readerFontPointSize
  w.u8(120);  // lineHeightPercent
  w.u8(2);    // wordSpacing
  w.u8(0);    // orientation
  w.u8(10);   // screenMarginVertical
  w.u8(12);   // screenMarginHorizontal
  w.u8(1);    // publisherPageNumbers
  w.u8(1);    // paragraphAlignment
  w.u8(0);    // embeddedStyle
  w.u8(1);    // hyphenationEnabled
  w.u8(1);    // textAntiAliasing
  w.u8(0);    // imageRendering
  w.u8(0);    // extraParagraphSpacing
  w.u8(1);    // forceParagraphIndents
  w.u8(0);    // focusReadingEnabled
  w.u8(0);    // guideReadingEnabled
  w.u8(0);    // epubRenderMode (CrossInkDefault)
  w.u8(1);    // indexingMethod
  w.fixed("", 64);  // sdFontFamilyName
}

ReaderSettingsSnapshot testDefaults() {
  ReaderSettingsSnapshot defaults;
  defaults.characterSpacing = 3;  // Distinctive: != the struct's own default-member value (2) and
                                   // != anything a fixture below writes, so a test that observes 3
                                   // in the result can only have gotten it via inheritance.
  return defaults;
}

}  // namespace

TEST(ReaderSettingsSnapshotIOTest, V11RoundTripPreservesEveryField) {
  BookReaderSettingsData written;
  written.hasAutoPageTurnInterval = true;
  written.autoPageTurnSeconds = 45;
  written.hasRenderModeOverride = true;
  written.renderMode = static_cast<uint8_t>(EpubRenderMode::Balanced);
  written.hasDictionaryFontOverride = true;
  std::strncpy(written.dictionarySdFontFamilyName, "MyDictFont", sizeof(written.dictionarySdFontFamilyName) - 1);
  written.dictionaryFontPointSize = 12;
  written.readerSettingsOverrideMask = ALL_READER_SETTING_OVERRIDES;
  written.readerSettings.fontFamily = 5;
  written.readerSettings.readerFontPointSize = 18;
  written.readerSettings.wordSpacing = 3;
  written.readerSettings.characterSpacing = 4;
  written.readerSettings.screenMarginVertical = 20;
  written.readerSettings.screenMarginHorizontal = 25;
  std::strncpy(written.readerSettings.sdFontFamilyName, "ReaderFont", sizeof(written.readerSettings.sdFontFamilyName) - 1);

  Storage.reset();
  const std::string path = "/book/reader_settings.bin";
  HalFile writeFile;
  ASSERT_TRUE(Storage.openFileForWrite("T", path, writeFile));
  ASSERT_TRUE(ReaderSettingsIO::writeBookReaderSettingsFile(writeFile, written));

  HalFile readFile;
  ASSERT_TRUE(Storage.openFileForRead("T", path, readFile));
  const BookReaderSettingsData result =
      ReaderSettingsIO::parseBookReaderSettingsFile(readFile, testDefaults(), "IgnoredDefault", 0);

  EXPECT_TRUE(result.hasAutoPageTurnInterval);
  EXPECT_EQ(result.autoPageTurnSeconds, 45);
  EXPECT_TRUE(result.hasRenderModeOverride);
  EXPECT_EQ(result.renderMode, static_cast<uint8_t>(EpubRenderMode::Balanced));
  EXPECT_TRUE(result.hasDictionaryFontOverride);
  EXPECT_STREQ(result.dictionarySdFontFamilyName, "MyDictFont");
  EXPECT_EQ(result.dictionaryFontPointSize, 12);
  EXPECT_TRUE(result.hasCustomReaderSettings);
  EXPECT_EQ(result.readerSettingsOverrideMask, ALL_READER_SETTING_OVERRIDES);
  EXPECT_EQ(result.readerSettings.fontFamily, 5);
  EXPECT_EQ(result.readerSettings.readerFontPointSize, 18);
  EXPECT_EQ(result.readerSettings.wordSpacing, 3);
  EXPECT_EQ(result.readerSettings.characterSpacing, 4);
  EXPECT_EQ(result.readerSettings.screenMarginVertical, 20);
  EXPECT_EQ(result.readerSettings.screenMarginHorizontal, 25);
  EXPECT_STREQ(result.readerSettings.sdFontFamilyName, "ReaderFont");
}

// The exact trap flagged in project memory: "Books saved before Character Spacing existed
// inherit the current global spacing." A v10 file predates the Character Spacing feature, so it
// can never have legitimately set CHARACTER_SPACING_OVERRIDE -- but this test writes that bit
// set anyway (an adversarial/malformed input) to prove the version-gated clear
// (`if (version < READER_SETTINGS_FILE_VERSION) mask &= ~CHARACTER_SPACING_OVERRIDE;`) actually
// runs, rather than assuming a "well-behaved" v10 file would never trigger the bug it guards
// against. A regular field (fontFamily) in the same override mask DOES apply, distinguishing
// "the whole override path is broken" from "only characterSpacing is specifically protected."
TEST(ReaderSettingsSnapshotIOTest, PreV11FileInheritsGlobalCharacterSpacingEvenWithOverrideBitSet) {
  RawWriter w;
  w.u8(ReaderSettingsIO::PRE_CHARACTER_SPACING_READER_SETTINGS_FILE_VERSION);  // version = 10
  w.u8(ReaderSettingsIO::READER_SETTINGS_FLAG_CUSTOM);                        // flags
  w.u16(0);                                                                   // autoPageTurnSeconds (unused)
  w.u8(0);                                                                    // renderMode (unused)
  appendV10PlusSnapshotBody(w);
  w.fixed("", 64);  // dictionarySdFontFamilyName (present since v10 >= PRE_POINT_SIZE(5))
  w.u8(0);          // dictionaryFontPointSize (present since v10 >= PRE_SPLIT_SCREEN_MARGIN(7))
  // Override mask: every field bit set, INCLUDING CHARACTER_SPACING_OVERRIDE -- the adversarial part.
  w.u32(ALL_READER_SETTING_OVERRIDES);
  // No trailing characterSpacing byte: that's what makes this a v10 file, not v11.

  Storage.reset();
  const std::string path = "/book/reader_settings.bin";
  Storage.put(path, w.bytes);

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path, file));
  const BookReaderSettingsData result = ReaderSettingsIO::parseBookReaderSettingsFile(file, testDefaults(), "", 0);

  EXPECT_TRUE(result.hasCustomReaderSettings);
  // The override mask must have CHARACTER_SPACING_OVERRIDE cleared even though the raw file set it.
  EXPECT_EQ(result.readerSettingsOverrideMask & CHARACTER_SPACING_OVERRIDE, 0u);
  // characterSpacing follows the caller-supplied default (3), NOT the fixture's snapshot body
  // (which never even encodes a character-spacing byte for a v10 file).
  EXPECT_EQ(result.readerSettings.characterSpacing, 3);
  // A regular field's override DID apply -- proves this isn't "overrides broken entirely."
  EXPECT_EQ(result.readerSettings.fontFamily, 3);
}

// Version 1 predates every per-book snapshot field entirely -- only an auto-page-turn interval.
// Everything else must come from the caller-supplied defaults, untouched.
TEST(ReaderSettingsSnapshotIOTest, LegacyV1FileOnlyCarriesAutoPageTurnInterval) {
  RawWriter w;
  w.u8(ReaderSettingsIO::LEGACY_READER_SETTINGS_FILE_VERSION);  // version = 1
  w.u16(30);                                                    // autoPageTurnSeconds

  Storage.reset();
  const std::string path = "/book/reader_settings.bin";
  Storage.put(path, w.bytes);

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path, file));
  const ReaderSettingsSnapshot defaults = testDefaults();
  const BookReaderSettingsData result = ReaderSettingsIO::parseBookReaderSettingsFile(file, defaults, "DefaultDict", 7);

  EXPECT_TRUE(result.hasAutoPageTurnInterval);
  EXPECT_EQ(result.autoPageTurnSeconds, 30);
  EXPECT_FALSE(result.hasCustomReaderSettings);
  EXPECT_EQ(result.readerSettings.characterSpacing, defaults.characterSpacing);
  EXPECT_EQ(result.readerSettings.fontFamily, defaults.fontFamily);
  EXPECT_STREQ(result.dictionarySdFontFamilyName, "DefaultDict");
  EXPECT_EQ(result.dictionaryFontPointSize, 7);
}

// A file truncated mid-record (a torn write, or an SD card fault) must fail safe: every field
// comes from the caller-supplied defaults, exactly as if the file did not exist, rather than
// reading whatever partial/garbage values happened to be in the truncated buffer.
TEST(ReaderSettingsSnapshotIOTest, TruncatedFileFallsBackEntirelyToDefaults) {
  BookReaderSettingsData written;
  written.readerSettingsOverrideMask = ALL_READER_SETTING_OVERRIDES;
  written.readerSettings.fontFamily = 9;
  written.readerSettings.characterSpacing = 4;

  Storage.reset();
  const std::string path = "/book/reader_settings.bin";
  HalFile writeFile;
  ASSERT_TRUE(Storage.openFileForWrite("T", path, writeFile));
  ASSERT_TRUE(ReaderSettingsIO::writeBookReaderSettingsFile(writeFile, written));

  // Cut the file down to just past the version byte -- everything else is missing.
  std::vector<uint8_t> fullBytes(Storage.bytes(path));
  ASSERT_GT(fullBytes.size(), 5u);
  Storage.put(path, std::vector<uint8_t>(fullBytes.begin(), fullBytes.begin() + 3));

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path, file));
  const ReaderSettingsSnapshot defaults = testDefaults();
  const BookReaderSettingsData result = ReaderSettingsIO::parseBookReaderSettingsFile(file, defaults, "DefaultDict", 7);

  EXPECT_FALSE(result.hasCustomReaderSettings);
  EXPECT_EQ(result.readerSettingsOverrideMask, 0u);
  EXPECT_EQ(result.readerSettings.characterSpacing, defaults.characterSpacing);
  EXPECT_EQ(result.readerSettings.fontFamily, defaults.fontFamily);
  EXPECT_STREQ(result.dictionarySdFontFamilyName, "DefaultDict");
}
