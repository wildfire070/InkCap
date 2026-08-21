#pragma once
#include <CompanionMood.h>

#include <cstdint>

#include "CompanionSprites.generated.h"

/**
 * @brief Runtime glue between the reader, the RTC, and CompanionState.
 *
 * Owns the per-session accumulator and the cached calendar day. Reading the RTC
 * costs an I2C transaction, so the day is resolved at session boundaries and
 * cached; currentMood() is safe to call from render paths.
 *
 * Sessions do not span deep sleep — waking is effectively a chip reset — so the
 * accumulator is banked into CompanionState before the reader exits.
 */
class CompanionTracker {
 public:
  static CompanionTracker& getInstance() {
    static CompanionTracker instance;
    return instance;
  }

  CompanionTracker(const CompanionTracker&) = delete;
  CompanionTracker& operator=(const CompanionTracker&) = delete;

  // True when the user has switched the companion on. Every hook is a no-op
  // otherwise, so the stock reader paths are untouched when disabled.
  static bool isEnabled();

  // Active character, clamped so a settings value from a newer firmware (or a
  // hand-edited settings.json) cannot index past the sprite table.
  static companion::CompanionId activeId();

  // Reader lifecycle. beginSession resolves the calendar day (one I2C read),
  // endSession banks credited time and persists if anything changed.
  void beginSession();
  void onPageTurn();
  void tick();
  void endSession();

  // Resolves the calendar day for screens that show the companion outside a
  // reading session (the home screen). Does one I2C read, so call it from a
  // lifecycle hook such as onEnter, never from a render path.
  void refreshForDisplay();

  // Cheap: uses the cached day, no I2C, no SD.
  companion::Mood currentMood() const;

  // Credited minutes attributed to today, including this session's not-yet
  // banked remainder. This is the same figure the mood is derived from, so a
  // progress hint built on it can never disagree with the pose on screen.
  uint16_t minutesToday() const;

  // False when the board has no RTC or it was never set. Day-based decay and
  // streaks are paused in that case; the UI can explain why.
  bool hasValidClock() const { return clockValid; }

 private:
  CompanionTracker() = default;

  // Reads the RTC and recomputes the cached local day. Does I2C.
  void refreshDay();

  // Single source for the mood inputs, so the pose and any figure shown beside
  // it are always derived from the same numbers.
  companion::MoodInput buildMoodInput() const;

  // Banks the accumulator into CompanionState. Returns true if state changed.
  bool bankSession();

  companion::SessionAccumulator accumulator;
  uint32_t pagesThisSession = 0;
  uint32_t bankedSeconds = 0;  // already folded into CompanionState this session
  int32_t localDay = 0;
  bool clockValid = false;
  bool sessionActive = false;
};

#define COMPANION CompanionTracker::getInstance()
