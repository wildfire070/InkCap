#include "CompanionMood.h"

namespace companion {

Mood evaluate(const MoodInput& in, const MoodThresholds& t) {
  if (in.creditedMinutesToday >= t.thrivingMinutes) return Mood::Thriving;

  // No clock: elapsed days are unknowable, so decay cannot be justified.
  // Content is the floor rather than punishing a user whose RTC was never set.
  if (!in.clockValid) return Mood::Content;

  if (in.creditedMinutesToday >= t.contentMinutes) return Mood::Content;
  // Read yesterday but not yet today: still within the day's grace.
  if (in.daysSinceLastRead <= 1) return Mood::Content;
  if (in.daysSinceLastRead < t.neglectedDays) return Mood::Peckish;
  return Mood::Neglected;
}

int32_t daysFromCivil(int32_t year, uint32_t month, uint32_t day) {
  // Shift the era so March starts the year, which makes the leap-day the last
  // day of the cycle and removes every February special case.
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(year - era * 400);              // [0, 399]
  const uint32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

int32_t daysBetween(int32_t fromYear, uint32_t fromMonth, uint32_t fromDay, int32_t toYear, uint32_t toMonth,
                    uint32_t toDay) {
  return daysFromCivil(toYear, toMonth, toDay) - daysFromCivil(fromYear, fromMonth, fromDay);
}

int32_t localDayNumber(int32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                       int32_t utcOffsetQuarterHours) {
  static constexpr int32_t MINUTES_PER_DAY = 1440;
  const int32_t utcMinutes = daysFromCivil(year, month, day) * MINUTES_PER_DAY +
                             static_cast<int32_t>(hour) * 60 + static_cast<int32_t>(minute);
  const int32_t localMinutes = utcMinutes + utcOffsetQuarterHours * 15;
  // Floor division: C++ truncates toward zero, which would put the pre-epoch
  // side of midnight on the wrong day.
  return localMinutes >= 0 ? localMinutes / MINUTES_PER_DAY
                           : -(((-localMinutes) + MINUTES_PER_DAY - 1) / MINUTES_PER_DAY);
}

void creditDay(DayLedger& ledger, const int32_t today, const uint16_t minutes, const uint16_t contentMinutes) {
  // A different day (in either direction: the clock can be corrected backwards)
  // starts a fresh counter rather than pouring today's minutes into a stale day.
  if (ledger.counterDay != today) {
    ledger.counterDay = today;
    ledger.minutesToday = 0;
  }

  const bool wasQualifying = ledger.minutesToday >= contentMinutes;
  // Saturate rather than wrap: a pathological session must not roll the counter
  // back to zero and hand out a fresh streak day.
  const uint32_t total = static_cast<uint32_t>(ledger.minutesToday) + minutes;
  ledger.minutesToday = total > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(total);

  if (wasQualifying || ledger.minutesToday < contentMinutes) return;

  // This day just cleared the bar for the first time: extend the streak when it
  // directly follows the previous qualifying day, otherwise start a new one.
  const bool consecutive = ledger.lastQualifyingDay != DayLedger::NEVER &&
                           today == ledger.lastQualifyingDay + 1;
  ledger.streakDays = consecutive && ledger.streakDays < UINT16_MAX ? ledger.streakDays + 1 : 1;
  if (ledger.streakDays > ledger.bestStreakDays) ledger.bestStreakDays = ledger.streakDays;
  ledger.lastQualifyingDay = today;
}

MoodInput moodInputFor(const DayLedger& ledger, const int32_t today, const bool clockValid,
                       const uint16_t clocklessSessionMinutes) {
  MoodInput in;
  in.clockValid = clockValid;

  if (!clockValid) {
    // Only this power session is knowable; evaluate() ignores daysSinceLastRead.
    in.creditedMinutesToday = clocklessSessionMinutes;
    return in;
  }

  in.creditedMinutesToday = ledger.counterDay == today ? ledger.minutesToday : 0;

  if (ledger.lastQualifyingDay == DayLedger::NEVER) {
    // Never read: a brand new companion has nothing to have neglected yet.
    in.daysSinceLastRead = 0;
    return in;
  }

  const int32_t elapsed = today - ledger.lastQualifyingDay;
  // A clock corrected backwards yields a negative span; treat it as "today"
  // rather than letting a wild value read as neglect.
  in.daysSinceLastRead = elapsed <= 0 ? 0 : static_cast<uint16_t>(elapsed > UINT16_MAX ? UINT16_MAX : elapsed);
  return in;
}

void SessionAccumulator::onPageTurn(const uint32_t nowSeconds) {
  if (!started) {
    started = true;
    lastTickS = nowSeconds;
  }
  lastPageTurnS = nowSeconds;
  sawPageTurn = true;
}

void SessionAccumulator::onTick(const uint32_t nowSeconds) {
  if (!started) {
    started = true;
    lastTickS = nowSeconds;
    return;
  }

  // A clock that jumped backwards (or wrapped) cannot yield a sane delta.
  // Re-anchor and bank nothing rather than crediting a bogus interval.
  if (nowSeconds < lastTickS) {
    lastTickS = nowSeconds;
    return;
  }

  uint32_t delta = nowSeconds - lastTickS;
  lastTickS = nowSeconds;

  if (!sawPageTurn) return;
  if (nowSeconds - lastPageTurnS > windowS) return;

  // Cap the delta at the active window: a longer gap means the loop was stalled
  // (or the caller tick was starved), not that the user read continuously.
  if (delta > windowS) delta = windowS;
  creditedS += delta;
}

void SessionAccumulator::reset() {
  lastTickS = 0;
  lastPageTurnS = 0;
  creditedS = 0;
  started = false;
  sawPageTurn = false;
}

}  // namespace companion
