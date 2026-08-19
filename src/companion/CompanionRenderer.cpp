#include "CompanionRenderer.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>

namespace companion {
namespace {

// Radius is at most 12, so a plain search beats pulling in <cmath> for sqrt.
int isqrt(const int value) {
  if (value <= 0) return 0;
  int root = 0;
  while ((root + 1) * (root + 1) <= value) root++;
  return root;
}

// Half-plane test fill. The tail is only ~12x20 px, so testing its bounding box
// is cheaper than sorting edges, and it is correct for either winding order.
void fillTriangle(const GfxRenderer& renderer, const int x0, const int y0, const int x1, const int y1, const int x2,
                  const int y2, const bool state) {
  const int minX = std::min({x0, x1, x2});
  const int maxX = std::max({x0, x1, x2});
  const int minY = std::min({y0, y1, y2});
  const int maxY = std::max({y0, y1, y2});

  const auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
  };

  for (int py = minY; py <= maxY; py++) {
    for (int px = minX; px <= maxX; px++) {
      const int e0 = edge(x0, y0, x1, y1, px, py);
      const int e1 = edge(x1, y1, x2, y2, px, py);
      const int e2 = edge(x2, y2, x0, y0, px, py);
      if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
        renderer.drawPixel(px, py, state);
      }
    }
  }
}

// Midpoint circle, plotting all four corner arcs of a rounded rect in one pass.
void strokeCornerArcs(const GfxRenderer& renderer, const int left, const int top, const int right, const int bottom,
                      const int radius) {
  int x = 0;
  int y = radius;
  int d = 1 - radius;
  while (x <= y) {
    const int lx = left + radius;
    const int rx = right - radius;
    const int ty = top + radius;
    const int by = bottom - radius;
    renderer.drawPixel(rx + x, by + y, true);
    renderer.drawPixel(rx + y, by + x, true);
    renderer.drawPixel(lx - x, by + y, true);
    renderer.drawPixel(lx - y, by + x, true);
    renderer.drawPixel(rx + x, ty - y, true);
    renderer.drawPixel(rx + y, ty - x, true);
    renderer.drawPixel(lx - x, ty - y, true);
    renderer.drawPixel(lx - y, ty - x, true);
    if (d < 0) {
      d += 2 * x + 3;
    } else {
      d += 2 * (x - y) + 5;
      y--;
    }
    x++;
  }
}

}  // namespace

void drawPose(const GfxRenderer& renderer, const CompanionId id, const Mood mood, const int x, const int y,
              const int scale, const bool mirrored) {
  if (scale < 1) return;

  const auto companionIndex = static_cast<uint8_t>(id);
  const auto moodIndex = static_cast<uint8_t>(mood);
  if (companionIndex >= COMPANION_COUNT || moodIndex >= MOOD_COUNT) {
    LOG_ERR("COMP", "Sprite index out of range: companion %u mood %u", companionIndex, moodIndex);
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const uint8_t* bits = COMPANION_SPRITES[companionIndex][moodIndex];

  for (int row = 0; row < SPRITE_HEIGHT; row++) {
    const uint8_t* rowBits = bits + row * SPRITE_ROW_BYTES;
    const int baseY = y + row * scale;
    // Whole sprite row lands off-screen: skip its columns entirely.
    if (baseY + scale <= 0 || baseY >= screenHeight) continue;

    for (int col = 0; col < SPRITE_WIDTH; col++) {
      // Mirroring reads the opposite column while writing to the same place, so
      // the flip costs nothing beyond one subtraction.
      const int srcCol = mirrored ? SPRITE_WIDTH - 1 - col : col;
      if (((rowBits[srcCol >> 3] >> (7 - (srcCol & 7))) & 1) == 0) continue;

      const int baseX = x + col * scale;
      if (baseX + scale <= 0 || baseX >= screenWidth) continue;

      for (int dy = 0; dy < scale; dy++) {
        const int py = baseY + dy;
        if (py < 0 || py >= screenHeight) continue;
        for (int dx = 0; dx < scale; dx++) {
          const int px = baseX + dx;
          if (px < 0 || px >= screenWidth) continue;
          renderer.drawPixel(px, py, true);
        }
      }
    }
  }
}

void drawSpeechBubble(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                      const int tailLength) {
  if (w <= 4 || h <= 4) return;

  const int radius = std::min({10, w / 3, h / 3});
  const int left = x;
  const int top = y;
  const int right = x + w - 1;
  const int bottom = y + h - 1;

  // Clear to paper first, following the rounded edge, so the text that follows
  // is never sitting on top of whatever was behind the bubble.
  for (int row = 0; row < h; row++) {
    int inset = 0;
    if (row < radius) {
      const int dy = radius - row;
      inset = radius - isqrt(radius * radius - dy * dy);
    } else if (row >= h - radius) {
      const int dy = row - (h - 1 - radius);
      inset = radius - isqrt(radius * radius - dy * dy);
    }
    renderer.fillRect(left + inset, top + row, w - 2 * inset, 1, false);
  }

  // Straight runs between the corner arcs.
  renderer.drawLine(left + radius, top, right - radius, top, true);
  renderer.drawLine(left + radius, bottom, right - radius, bottom, true);
  renderer.drawLine(left, top + radius, left, bottom - radius, true);
  renderer.drawLine(right, top + radius, right, bottom - radius, true);
  strokeCornerArcs(renderer, left, top, right, bottom, radius);

  if (tailLength <= 0) return;

  // Tail angled down-left, the way a comic bubble points at whoever is talking.
  // The base is kept near the tail's own length: a base much taller than the
  // reach reads as a shallow flap rather than a pointer.
  const int baseHalf = std::max(3, std::min(tailLength / 2, h / 8));
  const int midY = top + h / 2;
  const int baseTopY = midY - baseHalf;
  const int baseBottomY = midY + baseHalf;
  const int tipX = left - tailLength;
  const int tipY = baseBottomY + tailLength / 3;

  // Paper fill first: this also erases the body's left edge between the base
  // points, so the tail opens into the bubble instead of being a stuck-on shape.
  fillTriangle(renderer, left, baseTopY, left, baseBottomY, tipX, tipY, false);
  renderer.drawLine(left, baseTopY, tipX, tipY, true);
  renderer.drawLine(tipX, tipY, left, baseBottomY, true);
}

const char* moodLabel(const Mood mood) {
  switch (mood) {
    case Mood::Thriving:
      return tr(STR_COMPANION_MOOD_THRIVING);
    case Mood::Content:
      return tr(STR_COMPANION_MOOD_CONTENT);
    case Mood::Peckish:
      return tr(STR_COMPANION_MOOD_PECKISH);
    case Mood::Neglected:
      return tr(STR_COMPANION_MOOD_NEGLECTED);
  }
  return tr(STR_COMPANION_MOOD_CONTENT);
}

const char* quoteFor(const CompanionId id, const Mood mood, const uint32_t rotation) {
  const auto companionIndex = static_cast<uint8_t>(id);
  const auto moodIndex = static_cast<uint8_t>(mood);
  if (companionIndex >= COMPANION_COUNT || moodIndex >= MOOD_COUNT) return nullptr;

  const uint8_t count = COMPANION_QUOTE_COUNTS[companionIndex][moodIndex];
  if (count == 0) return nullptr;
  return COMPANION_QUOTES[companionIndex][moodIndex][rotation % count];
}

uint8_t quoteCountFor(const CompanionId id, const Mood mood) {
  const auto companionIndex = static_cast<uint8_t>(id);
  const auto moodIndex = static_cast<uint8_t>(mood);
  if (companionIndex >= COMPANION_COUNT || moodIndex >= MOOD_COUNT) return 0;
  return COMPANION_QUOTE_COUNTS[companionIndex][moodIndex];
}

const char* milestoneQuoteFor(const CompanionId id, const uint32_t rotation) {
  const auto companionIndex = static_cast<uint8_t>(id);
  if (companionIndex >= COMPANION_COUNT) return nullptr;

  const uint8_t count = COMPANION_MILESTONE_COUNTS[companionIndex];
  if (count == 0) return nullptr;
  return COMPANION_MILESTONE_QUOTES[companionIndex][rotation % count];
}

}  // namespace companion
