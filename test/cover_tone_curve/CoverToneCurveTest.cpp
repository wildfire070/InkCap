// The adaptive cover lift (lib/GfxRenderer/CoverToneCurve) decides, per cover,
// whether and how far to stretch luminance before dithering. Getting it wrong
// in one direction leaves dark covers as black blocks on the home screen; in
// the other it brightens covers that were fine. Both halves are pinned here.
//
// Reference for the "readable" bar: on the BW home/library screens only the
// top 2-bit level is white, and the quantizer puts that at source gray >= 140.

#include <gtest/gtest.h>

#include "lib/GfxRenderer/CoverToneCurve.h"

namespace {

constexpr int kWhiteOnBw = 140;  // AtkinsonDitherer level-3 threshold

// Fill `count` pixels at luminance `gray`.
void fill(LumaHistogram& h, const uint8_t gray, const int count) {
  for (int i = 0; i < count; i++) h.add(gray);
}

bool isIdentityTable(const CoverToneCurve& c) {
  for (int i = 0; i < 256; i++)
    if (c.apply(static_cast<uint8_t>(i)) != i) return false;
  return true;
}

bool isMonotone(const CoverToneCurve& c) {
  for (int i = 1; i < 256; i++)
    if (c.apply(static_cast<uint8_t>(i)) < c.apply(static_cast<uint8_t>(i - 1))) return false;
  return true;
}

TEST(CoverToneCurveTest, EmptyHistogramIsIdentity) {
  LumaHistogram h;
  CoverToneCurve c;
  c.build(h);
  EXPECT_TRUE(c.isIdentity());
  EXPECT_TRUE(isIdentityTable(c));
}

// White background, black text: the classic light cover. Must not move.
TEST(CoverToneCurveTest, LightCoverUntouched) {
  LumaHistogram h;
  fill(h, 245, 9000);
  fill(h, 10, 1000);
  CoverToneCurve c;
  c.build(h);
  EXPECT_TRUE(c.isIdentity());
  EXPECT_TRUE(isIdentityTable(c));
}

// Pale cream cover with grey text (everything light, nothing at 255).
TEST(CoverToneCurveTest, PaleCoverUntouched) {
  LumaHistogram h;
  fill(h, 215, 8000);
  fill(h, 120, 2000);
  CoverToneCurve c;
  c.build(h);
  EXPECT_TRUE(c.isIdentity());
}

// Dark background with a mid-grey title: the complaint. The title must clear
// the BW white threshold; the background must stay well below it.
TEST(CoverToneCurveTest, DarkCoverTitleBecomesReadable) {
  LumaHistogram h;
  fill(h, 20, 8500);
  fill(h, 100, 1500);
  CoverToneCurve c;
  c.build(h);
  EXPECT_FALSE(c.isIdentity());
  EXPECT_GE(c.apply(100), kWhiteOnBw);
  EXPECT_LT(c.apply(20), 64);
  EXPECT_TRUE(isMonotone(c));
  EXPECT_EQ(c.apply(0), 0);
  EXPECT_EQ(c.apply(255), 255);
}

// Dark background with an already-white title is readable as it is: the
// bright percentile sits at the top, so there is nothing to stretch.
TEST(CoverToneCurveTest, DarkCoverWhiteTitleUntouched) {
  LumaHistogram h;
  fill(h, 20, 8500);
  fill(h, 250, 1500);
  CoverToneCurve c;
  c.build(h);
  EXPECT_TRUE(c.isIdentity());
}

// Near-black cover: the gain is capped, so it brightens "a little" rather than
// turning sensor noise into a white field.
TEST(CoverToneCurveTest, NearBlackCoverCapped) {
  LumaHistogram h;
  fill(h, 5, 6000);
  fill(h, 30, 4000);
  CoverToneCurve c;
  c.build(h);
  EXPECT_FALSE(c.isIdentity());
  EXPECT_EQ(c.gain256(), CoverToneCurve::kMaxGain256);
  EXPECT_LT(c.apply(30), kWhiteOnBw);
  EXPECT_GT(c.apply(30), 30);
}

// Mid-tone photographic cover: a partial lift, strictly weaker than the full one.
TEST(CoverToneCurveTest, MidToneCoverGetsPartialLift) {
  LumaHistogram full, mid;
  fill(full, 30, 7000);
  fill(full, 150, 3000);  // mean ~66 -> full strength
  fill(mid, 90, 7000);
  fill(mid, 180, 3000);  // mean ~117 -> partial strength
  CoverToneCurve cf, cm;
  cf.build(full);
  cm.build(mid);
  EXPECT_FALSE(cf.isIdentity());
  EXPECT_FALSE(cm.isIdentity());
  EXPECT_GT(cm.gain256(), 256);
  EXPECT_LT(cm.gain256(), cf.gain256());
  EXPECT_TRUE(isMonotone(cm));
}

// A rebuilt curve must forget the previous cover entirely.
TEST(CoverToneCurveTest, RebuildResetsToIdentity) {
  LumaHistogram dark, light;
  fill(dark, 20, 9000);
  fill(dark, 100, 1000);
  fill(light, 240, 10000);
  CoverToneCurve c;
  c.build(dark);
  EXPECT_FALSE(c.isIdentity());
  c.build(light);
  EXPECT_TRUE(c.isIdentity());
  EXPECT_TRUE(isIdentityTable(c));
}

TEST(CoverToneCurveTest, HistogramStats) {
  LumaHistogram h;
  fill(h, 0, 50);
  fill(h, 255, 50);
  EXPECT_EQ(h.total(), 100u);
  EXPECT_EQ(h.percentileBin(2), 0);
  EXPECT_EQ(h.percentileBin(50), 0);
  EXPECT_EQ(h.percentileBin(51), LumaHistogram::kBins - 1);
  EXPECT_EQ(h.percentileBin(90), LumaHistogram::kBins - 1);
  // Bin centres: (4 + 252) / 2
  EXPECT_EQ(h.mean(), 128);
}

}  // namespace
