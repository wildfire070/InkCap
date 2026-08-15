#include <gtest/gtest.h>

#include "src/util/SwipeAdjustment.h"

namespace {

SwipeAdjustment::Swipe swipe(const uint8_t fingers, const int startX, const int startY, const int endX, const int endY,
                             const unsigned long durationMs) {
  return {fingers, startX, startY, endX, endY, durationMs};
}

}  // namespace

TEST(SwipeAdjustment, RejectsOffAndInvalidBindings) {
  SwipeAdjustment::DecodedBinding decoded;
  EXPECT_FALSE(SwipeAdjustment::decodeBinding(SwipeAdjustment::Binding::Off, decoded));
  EXPECT_FALSE(SwipeAdjustment::decodeBinding(255, decoded));
  EXPECT_FALSE(SwipeAdjustment::bindingsConflict(SwipeAdjustment::Binding::Off, SwipeAdjustment::Binding::Off));
  EXPECT_TRUE(SwipeAdjustment::bindingsConflict(SwipeAdjustment::Binding::ThreeFingerHorizontal,
                                                SwipeAdjustment::Binding::ThreeFingerHorizontal));
  EXPECT_FALSE(SwipeAdjustment::bindingsConflict(SwipeAdjustment::Binding::TwoFingerVertical,
                                                 SwipeAdjustment::Binding::TwoFingerHorizontal));
}

TEST(SwipeAdjustment, RequiresTheExactConfiguredFingerCount) {
  int delta = 0;
  EXPECT_FALSE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(3, 100, 700, 100, 500, 400),
                                         480, 800, delta));
}

TEST(SwipeAdjustment, MapsDirectionsToSignedDeltas) {
  int delta = 0;
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 700, 100, 500, 400),
                                        480, 800, delta));
  EXPECT_GT(delta, 0);
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerHorizontal,
                                        swipe(2, 100, 300, 300, 300, 400), 480, 800, delta));
  EXPECT_GT(delta, 0);
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 500, 100, 700, 400),
                                        480, 800, delta));
  EXPECT_LT(delta, 0);
}

TEST(SwipeAdjustment, RejectsShortDiagonalAndLateGestures) {
  int delta = 0;
  EXPECT_FALSE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 300, 100, 245, 400),
                                         480, 800, delta));
  EXPECT_FALSE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 700, 260, 560, 400),
                                         480, 800, delta));
  EXPECT_FALSE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical,
                                         swipe(2, 100, 700, 100, 500, 2001), 480, 800, delta));
}

TEST(SwipeAdjustment, NormalizesDistanceAcrossScreenSizes) {
  int smallDelta = 0;
  int largeDelta = 0;
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 50, 360, 50, 300, 1000),
                                        240, 400, smallDelta));
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 720, 100, 600, 1000),
                                        480, 800, largeDelta));
  EXPECT_EQ(smallDelta, largeDelta);
}

TEST(SwipeAdjustment, FastSwipesHaveLargerMagnitude) {
  int slowDelta = 0;
  int fastDelta = 0;
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 700, 100, 500, 1000),
                                        480, 800, slowDelta));
  EXPECT_TRUE(SwipeAdjustment::evaluate(SwipeAdjustment::Binding::TwoFingerVertical, swipe(2, 100, 700, 100, 500, 100),
                                        480, 800, fastDelta));
  EXPECT_GT(fastDelta, slowDelta);
}

TEST(SwipeAdjustment, ClampsValuesAndGatesWarmthByCapability) {
  uint8_t value = 98;
  EXPECT_TRUE(SwipeAdjustment::applyDelta(value, 10));
  EXPECT_EQ(value, 100);
  EXPECT_FALSE(SwipeAdjustment::applyDelta(value, 10));
  EXPECT_TRUE(SwipeAdjustment::targetAvailable(SwipeAdjustment::Target::Brightness, true, false));
  EXPECT_FALSE(SwipeAdjustment::targetAvailable(SwipeAdjustment::Target::Warmth, true, false));
  EXPECT_TRUE(SwipeAdjustment::targetAvailable(SwipeAdjustment::Target::Warmth, true, true));
}
