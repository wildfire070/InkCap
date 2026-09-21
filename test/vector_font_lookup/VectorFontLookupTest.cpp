// A runtime-rasterized (TTF) font has no interval table, so its glyphs only exist once the miss
// handler faults them in. EpdFontFamily's probing lookups (findGlyphData, used for fallback and
// "does this font really have this glyph" decisions) call EpdFont::findGlyph() only -- if findGlyph()
// never reaches the miss handler, every codepoint reads as missing and the renderer draws tofu.

#include <gtest/gtest.h>

#include <cstring>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"
#include "SdCardFontRegistry.h"

namespace {

EpdGlyph gGlyphA{};
int gMissCalls = 0;

const EpdGlyph* missHandler(void* /*ctx*/, const uint32_t cp) {
  ++gMissCalls;
  return cp == 'A' ? &gGlyphA : nullptr;
}

const uint8_t* vectorBitmap(void* /*ctx*/, const EpdGlyph* /*glyph*/) { return nullptr; }

EpdFontData makeData(const bool vector) {
  EpdFontData data;
  std::memset(&data, 0, sizeof(data));
  data.glyphMissHandler = &missHandler;
  if (vector) data.vectorBitmapHandler = &vectorBitmap;
  return data;
}

TEST(VectorFontLookupTest, FindGlyphFaultsVectorGlyphsThroughTheMissHandler) {
  gMissCalls = 0;
  EpdFontData data = makeData(/*vector=*/true);
  const EpdFont font(&data);

  EXPECT_EQ(font.findGlyph('A'), &gGlyphA);
  EXPECT_EQ(font.findGlyph('B'), nullptr);  // not in the face: still reads as missing
  EXPECT_EQ(gMissCalls, 2);
}

TEST(VectorFontLookupTest, FamilyProbeSeesVectorGlyphs) {
  EpdFontData data = makeData(/*vector=*/true);
  const EpdFont font(&data);
  const EpdFontFamily family(&font);

  EXPECT_EQ(family.findGlyphData('A', EpdFontFamily::REGULAR).glyph, &gGlyphA);
  EXPECT_EQ(family.getFallbackCodepoint('A', EpdFontFamily::REGULAR), static_cast<uint32_t>('A'));
}

TEST(VectorFontLookupTest, GetGlyphDoesNotFaultAMissTwiceForVectorFonts) {
  gMissCalls = 0;
  EpdFontData data = makeData(/*vector=*/true);
  const EpdFont font(&data);

  EXPECT_EQ(font.getGlyph('A'), &gGlyphA);
  EXPECT_EQ(gMissCalls, 1);
}

TEST(VectorFontLookupTest, FindGlyphStaysSideEffectFreeForNonVectorFonts) {
  // SD (.cpfont) fonts have a miss handler too, but their findGlyph() is a pure table probe: the
  // handler runs only from getGlyph().
  gMissCalls = 0;
  EpdFontData data = makeData(/*vector=*/false);
  const EpdFont font(&data);

  EXPECT_EQ(font.findGlyph('A'), nullptr);
  EXPECT_EQ(gMissCalls, 0);
  EXPECT_EQ(font.getGlyph('A'), &gGlyphA);
  EXPECT_EQ(gMissCalls, 1);
}

TEST(VectorFontSizeSnapTest, KeepsAnOfferedSizeUnchanged) {
  // Regression: the loader used to overwrite its own target while scanning and always landed on 9pt.
  const std::vector<uint8_t> steps{8, 9, 10, 12, 14, 16, 18, 20};
  for (const uint8_t size : steps) EXPECT_EQ(closestPointSize(steps, size), size);
}

TEST(VectorFontSizeSnapTest, SnapsBetweenStepsAndBreaksTiesToTheSmaller) {
  const std::vector<uint8_t> steps{8, 9, 10, 12, 14, 16, 18, 20};
  EXPECT_EQ(closestPointSize(steps, 11), 10);  // 10 and 12 are equally close
  EXPECT_EQ(closestPointSize(steps, 13), 12);
  EXPECT_EQ(closestPointSize(steps, 15), 14);
  EXPECT_EQ(closestPointSize(steps, 19), 18);
}

TEST(VectorFontSizeSnapTest, ClampsOutsideTheRangeAndPassesThroughWhenEmpty) {
  const std::vector<uint8_t> steps{8, 9, 10, 12, 14, 16, 18, 20};
  EXPECT_EQ(closestPointSize(steps, 5), 8);
  EXPECT_EQ(closestPointSize(steps, 40), 20);
  EXPECT_EQ(closestPointSize({}, 14), 14);
}

}  // namespace
