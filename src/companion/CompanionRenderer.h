#pragma once
#include <CompanionMood.h>
#include <GfxRenderer.h>

#include "CompanionSprites.generated.h"

namespace companion {

// Logical footprint of a pose at the given integer scale, for layout maths.
constexpr int poseWidth(const int scale) { return SPRITE_WIDTH * scale; }
constexpr int poseHeight(const int scale) { return SPRITE_HEIGHT * scale; }

/**
 * Draws one companion pose with its top-left at (x, y), magnified by an integer
 * scale (1 = one framebuffer pixel per sprite pixel).
 *
 * Every pixel goes through GfxRenderer::drawPixel, so the renderer's own
 * coordinate transform handles all four screen orientations and no sprite data
 * has to be pre-rotated. Pixels outside the screen are dropped here rather than
 * by drawPixel, which logs an error per out-of-bounds write.
 *
 * Nothing is scaled by fractions: e-ink at this size needs hard edges, and
 * integer scaling keeps the baked dither pattern intact.
 */
// `mirrored` flips the sprite horizontally so a walking character can face the
// way it is travelling. The sprites are symmetric enough to read either way, so
// no second set of art is needed.
void drawPose(const GfxRenderer& renderer, CompanionId id, Mood mood, int x, int y, int scale, bool mirrored = false);

// Picks one of a companion's lines for the given mood. `rotation` is wrapped
// into range, so callers can pass any counter (a visit count, a day number)
// without knowing how many lines a character has. Returns nullptr when the
// indices are out of range.
const char* quoteFor(CompanionId id, Mood mood, uint32_t rotation);

// How many lines a companion has for a mood, so a caller can pick an index
// itself (to avoid repeating the previous one) rather than only rotating.
uint8_t quoteCountFor(CompanionId id, Mood mood);

// Which edge the tail hangs off, so the bubble can point at a companion beside
// it or below it.
enum class TailSide : uint8_t { Left, Bottom };

/**
 * Draws a rounded speech bubble with a tail pointing at the speaker.
 *
 * The interior is cleared to paper before the outline is stroked, so callers can
 * draw text straight afterwards without worrying about what was underneath.
 * `tailLength` is how far the tail reaches beyond the bubble body.
 */
void drawSpeechBubble(const GfxRenderer& renderer, int x, int y, int w, int h, int tailLength,
                      TailSide side = TailSide::Left);

// Translated name of a mood, for the status label under the character. Unlike
// the character quotes this is UI chrome, so it comes from the string table.
const char* moodLabel(Mood mood);

// The character's one-off line for beating their own best streak.
const char* milestoneQuoteFor(CompanionId id, uint32_t rotation);

}  // namespace companion
