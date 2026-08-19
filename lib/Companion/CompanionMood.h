#pragma once

#include <cstdint>

// Reading-driven mood model for the companion. Deliberately free of Arduino,
// FreeRTOS, and HAL includes so the whole decay/credit policy is exercised by
// host unit tests before it ever reaches the device.
namespace companion {

enum class Mood : uint8_t { Thriving = 0, Content = 1, Peckish = 2, Neglected = 3 };

// Tunables kept in one struct so tests can pin behaviour without rebuilding the
// firmware defaults.
struct MoodThresholds {
  uint16_t thrivingMinutes = 25;  // credited minutes today that earn Thriving
  uint16_t contentMinutes = 2;    // credited minutes today that count as "read today"
  uint8_t neglectedDays = 3;      // quiet days at or above which the mood bottoms out
};

struct MoodInput {
  uint16_t creditedMinutesToday = 0;
  // Whole calendar days between the last day with credited reading and today.
  // 0 = read today, 1 = read yesterday, 2 = skipped one full day.
  uint16_t daysSinceLastRead = 0;
  // False when the RTC is absent or was never set (see HalClock::getDate).
  // Day arithmetic is meaningless then, so the decay ladder is skipped.
  bool clockValid = false;
};

// Maps reading activity onto one of the four drawn poses.
//
// With a valid clock the ladder is: enough minutes today -> Thriving, some
// reading today or yesterday -> Content, one skipped day -> Peckish, and
// neglectedDays or more -> Neglected.
//
// Without a clock, elapsed days cannot be measured, so neglect is
// unknowable and the result never falls below Content. Mood then reflects only
// the current power session, which is the honest reading of what is knowable.
Mood evaluate(const MoodInput& in, const MoodThresholds& t = {});

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Integer-only: no <ctime>, no 64-bit division, and valid
// well past any date an e-reader RTC will report.
int32_t daysFromCivil(int32_t year, uint32_t month, uint32_t day);

// Whole days from the first date to the second. Negative when the second date
// is earlier, which the caller should treat as a clock that moved backwards.
int32_t daysBetween(int32_t fromYear, uint32_t fromMonth, uint32_t fromDay, int32_t toYear, uint32_t toMonth,
                    uint32_t toDay);

// Local day number (days since 1970-01-01 in the user's own zone) for a UTC
// wall-clock reading. The RTC stores UTC, but a reading day must roll over at
// the user's midnight, not UTC's -- otherwise a late-evening session west of
// Greenwich lands on tomorrow and silently breaks a streak.
//
// This one integer is the whole day key: equality means "same day" and
// subtraction gives elapsed days, so no reverse calendar conversion is needed.
// utcOffsetQuarterHours is signed (UTC+0 = 0, UTC-5 = -20, UTC+14 = 56).
int32_t localDayNumber(int32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                       int32_t utcOffsetQuarterHours);

/**
 * @brief Per-day reading counters and streak bookkeeping.
 *
 * Kept here rather than inside the persisted store so every day-rollover and
 * streak transition is host-tested; CompanionState only serialises these fields.
 *
 * Two day markers are tracked because they answer different questions:
 * counterDay says which day minutesToday belongs to, while lastQualifyingDay is
 * the last day that actually cleared contentMinutes. Collapsing them would let a
 * few seconds of page-flipping reset the neglect clock.
 */
struct DayLedger {
  // Sentinel for "no reading has ever been credited".
  static constexpr int32_t NEVER = INT32_MIN;

  int32_t counterDay = NEVER;          // local day that minutesToday belongs to
  int32_t lastQualifyingDay = NEVER;   // last local day that cleared contentMinutes
  uint16_t minutesToday = 0;
  uint16_t streakDays = 0;
  uint16_t bestStreakDays = 0;
};

// Adds credited minutes to `today`, rolling counters over at the day boundary
// and extending (or restarting) the streak the first time a day qualifies.
// A day that never clears contentMinutes leaves the streak and neglect clock
// untouched.
void creditDay(DayLedger& ledger, int32_t today, uint16_t minutes, uint16_t contentMinutes);

// Derives the mood inputs for `today` from a ledger. When the clock is invalid,
// day arithmetic is skipped and only the current session's minutes are used.
MoodInput moodInputFor(const DayLedger& ledger, int32_t today, bool clockValid, uint16_t clocklessSessionMinutes);

/**
 * @brief Credits reading time only while pages are actually turning.
 *
 * A reader left open on one page is not reading, and a device that flips pages
 * without pausing is not either. Time is banked only while a page turn happened
 * within the trailing active window, so both idling and flip-spamming stop
 * earning credit.
 *
 * Time source is monotonic seconds within one power session (millis()/1000 on
 * device). Wake from deep sleep resets the MCU, so an accumulator never spans a
 * sleep: the caller banks creditedSeconds() into persistent state before sleeping.
 */
class SessionAccumulator {
 public:
  static constexpr uint16_t DEFAULT_ACTIVE_WINDOW_S = 300;

  explicit SessionAccumulator(uint16_t activeWindowSeconds = DEFAULT_ACTIVE_WINDOW_S)
      : windowS(activeWindowSeconds) {}

  // Marks the session active; time keeps accruing for activeWindowSeconds.
  void onPageTurn(uint32_t nowSeconds);

  // Banks elapsed time since the previous call when the session is active.
  // Safe to call at any cadence; the reader calls it from its activity loop.
  void onTick(uint32_t nowSeconds);

  uint32_t creditedSeconds() const { return creditedS; }
  uint16_t creditedMinutes() const { return static_cast<uint16_t>(creditedS / 60); }

  void reset();

 private:
  uint32_t lastTickS = 0;
  uint32_t lastPageTurnS = 0;
  uint32_t creditedS = 0;
  uint16_t windowS;
  bool started = false;
  bool sawPageTurn = false;
};

}  // namespace companion
