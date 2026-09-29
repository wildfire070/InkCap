#include <gtest/gtest.h>

#include "DictionaryWordMeasure.h"
#include "GfxRenderer.h"

using DictionaryWordMeasure::measureWordAdvanceX;
using DictionaryWordMeasure::withoutSoftHyphens;

namespace {
constexpr EpdFontFamily::Style REGULAR = EpdFontFamily::REGULAR;
}  // namespace

// The exact bug class fixed on 2026-09-26 ("DictionaryWordSelectActivity measured words without
// the block's tracking, so selection boxes drifted from the drawn words"): word width must
// widen with tracking exactly like the renderer's own draw path does (extra pixels between each
// pair of adjacent glyphs, so N glyphs get N-1 gaps).
TEST(DictionaryWordMeasureTest, PlainMeasurementAppliesTrackingBetweenGlyphs) {
  GfxRenderer renderer;
  char scratch[16];

  EXPECT_EQ(measureWordAdvanceX(renderer, 0, "abc", 3, REGULAR, 0, scratch, sizeof(scratch)), 3);
  EXPECT_EQ(measureWordAdvanceX(renderer, 0, "abc", 3, REGULAR, 2, scratch, sizeof(scratch)), 3 + 2 * 2);
}

// A soft hyphen (U+00AD) must be stripped before measuring -- otherwise the returned width
// includes a glyph advance the renderer never draws (ParsedText strips these at layout time too).
// Compares against the SAME stub measuring the un-stripped string directly, so this only passes
// if withoutSoftHyphens() actually ran, not because the two happen to coincide.
TEST(DictionaryWordMeasureTest, StripsSoftHyphenBeforeMeasuring) {
  GfxRenderer renderer;
  char scratch[16];
  // String-literal-concatenated so the hex escape doesn't greedily consume 'b'/'c' as hex digits.
  const char withHyphen[] = "a\xC2\xAD" "bc";  // 'a', soft hyphen (2 bytes), 'b', 'c' -- 5 bytes total.

  const int16_t stripped = measureWordAdvanceX(renderer, 0, withHyphen, sizeof(withHyphen) - 1, REGULAR, 0, scratch,
                                                sizeof(scratch));
  const int unstrippedRaw = renderer.getTextAdvanceX(0, withHyphen, REGULAR);

  EXPECT_EQ(stripped, 3);          // "abc": 3 glyphs, 0 tracking.
  EXPECT_EQ(unstrippedRaw, 4);     // 'a' + one 2-byte codepoint + 'b' + 'c' = 4 "glyphs" unstripped.
  EXPECT_NE(stripped, unstrippedRaw);
}

// focusBoundary/focusSuffixX overload: the highlighted prefix's width is already known
// (focusSuffixX), so only the un-highlighted suffix past focusBoundary is measured -- and that
// measurement must still be tracking-aware.
TEST(DictionaryWordMeasureTest, FocusBoundaryOverloadMeasuresOnlyTheSuffixWithTracking) {
  GfxRenderer renderer;
  char scratch[16];
  const char word[] = "abcdef";  // focusBoundary=3 splits into "abc" | "def".

  const int16_t noTracking =
      measureWordAdvanceX(renderer, 0, word, 6, REGULAR, 0, /*focusBoundary=*/3, /*focusSuffixX=*/100, scratch,
                          sizeof(scratch));
  EXPECT_EQ(noTracking, 100 + 3);  // prefix width (100, given) + "def" (3 glyphs, 0 tracking).

  const int16_t withTracking =
      measureWordAdvanceX(renderer, 0, word, 6, REGULAR, 2, /*focusBoundary=*/3, /*focusSuffixX=*/100, scratch,
                          sizeof(scratch));
  EXPECT_EQ(withTracking, 100 + 3 + 2 * 2);  // + tracking between "def"'s 3 glyphs.
}

// focusBoundary==0 or focusSuffixX==0 means "no focus split" -- must fall back to the plain
// whole-word measurement, not silently return a partial/zero result.
TEST(DictionaryWordMeasureTest, FocusBoundaryOverloadFallsBackToPlainWhenNoSplit) {
  GfxRenderer renderer;
  char scratch[16];
  const char word[] = "abcdef";

  const int16_t viaFallback =
      measureWordAdvanceX(renderer, 0, word, 6, REGULAR, 2, /*focusBoundary=*/0, /*focusSuffixX=*/100, scratch,
                          sizeof(scratch));
  const int16_t viaPlain = measureWordAdvanceX(renderer, 0, word, 6, REGULAR, 2, scratch, sizeof(scratch));
  EXPECT_EQ(viaFallback, viaPlain);
}

// RTL focus overload: when the word is RTL and both a boundary and offset are given, the bold
// prefix is measured as its own tracked run (not derived from the LTR "prefix width" shortcut).
TEST(DictionaryWordMeasureTest, RtlOverloadMeasuresBoldPrefixWithTracking) {
  GfxRenderer renderer;
  char scratch[16];
  const char word[] = "abcdefgh";  // focusBoundary=3 -> bold prefix "abc".

  const int16_t rtl = measureWordAdvanceX(renderer, 0, word, 8, REGULAR, 1, /*focusBoundary=*/3,
                                          /*focusRunOffset=*/50, /*wordIsRtl=*/true, scratch, sizeof(scratch));
  EXPECT_EQ(rtl, 50 + 3 + 1 * 2);  // offset (given) + "abc" (3 glyphs, tracking 1 between each pair).
}

// A non-RTL word (or a missing boundary/offset) must fall back to the LTR focus-suffix overload
// instead of taking the RTL bold-prefix path -- distinct offset (200 vs 50) so a wrong branch
// produces an obviously wrong number rather than an accidental match.
TEST(DictionaryWordMeasureTest, RtlOverloadFallsBackToLtrSuffixWhenNotRtl) {
  GfxRenderer renderer;
  char scratch[16];
  const char word[] = "abcdefgh";

  const int16_t viaFallback = measureWordAdvanceX(renderer, 0, word, 8, REGULAR, 1, /*focusBoundary=*/3,
                                                   /*focusRunOffset=*/200, /*wordIsRtl=*/false, scratch,
                                                   sizeof(scratch));
  const int16_t viaLtrSuffix =
      measureWordAdvanceX(renderer, 0, word, 8, REGULAR, 1, /*focusBoundary=*/3, /*focusSuffixX=*/200, scratch,
                          sizeof(scratch));
  EXPECT_EQ(viaFallback, viaLtrSuffix);
}

TEST(DictionaryWordMeasureTest, WithoutSoftHyphensReturnsOriginalPointerWhenNonePresent) {
  char scratch[16];
  const char* word = "plainword";
  EXPECT_EQ(withoutSoftHyphens(word, 9, scratch, sizeof(scratch)), word);
}
