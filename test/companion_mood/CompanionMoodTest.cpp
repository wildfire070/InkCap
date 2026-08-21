#include <gtest/gtest.h>

#include <set>

#include "Companion/CompanionMood.h"

using companion::Mood;
using companion::MoodInput;
using companion::MoodThresholds;
using companion::SessionAccumulator;

namespace {

MoodInput withClock(uint16_t minutesToday, uint16_t daysSince) {
  MoodInput in;
  in.creditedMinutesToday = minutesToday;
  in.daysSinceLastRead = daysSince;
  in.clockValid = true;
  return in;
}

}  // namespace

// ---------------------------------------------------------------- mood ladder

TEST(CompanionMood, LongSessionTodayIsThriving) {
  EXPECT_EQ(companion::evaluate(withClock(40, 0)), Mood::Thriving);
  EXPECT_EQ(companion::evaluate(withClock(120, 0)), Mood::Thriving);
}

TEST(CompanionMood, JustUnderThrivingIsStillContent) {
  // Derived from the threshold rather than hardcoded, so retuning the target
  // does not require editing the test that guards its boundary.
  const MoodThresholds t;
  EXPECT_EQ(companion::evaluate(withClock(t.thrivingMinutes - 1, 0)), Mood::Content);
  EXPECT_EQ(companion::evaluate(withClock(t.thrivingMinutes, 0)), Mood::Thriving);
}

TEST(CompanionMood, AnyRealReadingTodayIsContent) {
  EXPECT_EQ(companion::evaluate(withClock(2, 0)), Mood::Content);
}

TEST(CompanionMood, ReadYesterdayKeepsContentGrace) {
  // Nothing yet today, but yesterday counted: no penalty until a day is skipped.
  EXPECT_EQ(companion::evaluate(withClock(0, 1)), Mood::Content);
}

TEST(CompanionMood, OneSkippedDayIsPeckish) {
  EXPECT_EQ(companion::evaluate(withClock(0, 2)), Mood::Peckish);
}

TEST(CompanionMood, ThreeQuietDaysIsNeglected) {
  EXPECT_EQ(companion::evaluate(withClock(0, 3)), Mood::Neglected);
  EXPECT_EQ(companion::evaluate(withClock(0, 90)), Mood::Neglected);
}

TEST(CompanionMood, NeglectIsRecoverableWithinOneSession) {
  // The chosen design has no death state: a long read from the floor goes
  // straight back to the top.
  EXPECT_EQ(companion::evaluate(withClock(0, 400)), Mood::Neglected);
  EXPECT_EQ(companion::evaluate(withClock(40, 400)), Mood::Thriving);
}

TEST(CompanionMood, TrivialReadingDoesNotClearASkippedDay) {
  // A few seconds of page-flipping is below contentMinutes, so the decay ladder
  // still applies and the skipped day is not laundered away.
  EXPECT_EQ(companion::evaluate(withClock(1, 2)), Mood::Peckish);
}

TEST(CompanionMood, ThresholdsAreConfigurable) {
  MoodThresholds relaxed;
  relaxed.thrivingMinutes = 10;
  relaxed.neglectedDays = 7;
  EXPECT_EQ(companion::evaluate(withClock(10, 0), relaxed), Mood::Thriving);
  EXPECT_EQ(companion::evaluate(withClock(0, 5), relaxed), Mood::Peckish);
  EXPECT_EQ(companion::evaluate(withClock(0, 7), relaxed), Mood::Neglected);
}

// -------------------------------------------------------- clockless fallback

TEST(CompanionMood, WithoutClockNeverFallsBelowContent) {
  MoodInput in;
  in.clockValid = false;
  in.creditedMinutesToday = 0;
  in.daysSinceLastRead = 999;  // garbage without a clock; must be ignored
  EXPECT_EQ(companion::evaluate(in), Mood::Content);
}

TEST(CompanionMood, WithoutClockThrivingStillReachable) {
  MoodInput in;
  in.clockValid = false;
  in.creditedMinutesToday = 45;
  EXPECT_EQ(companion::evaluate(in), Mood::Thriving);
}

// ------------------------------------------------------------ calendar maths

TEST(CompanionCalendar, EpochIsDayZero) { EXPECT_EQ(companion::daysFromCivil(1970, 1, 1), 0); }

TEST(CompanionCalendar, KnownDates) {
  EXPECT_EQ(companion::daysFromCivil(1969, 12, 31), -1);
  EXPECT_EQ(companion::daysFromCivil(1970, 1, 2), 1);
  EXPECT_EQ(companion::daysFromCivil(2000, 3, 1), 11017);
  EXPECT_EQ(companion::daysFromCivil(2026, 8, 18), 20683);
}

TEST(CompanionCalendar, ConsecutiveDaysAlwaysDifferByOne) {
  // Walks several years day by day, which catches month-length and leap-year
  // errors that spot checks miss.
  static constexpr uint8_t kLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int32_t previous = companion::daysFromCivil(2023, 1, 1);
  for (int32_t y = 2023; y <= 2029; ++y) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    for (uint32_t m = 1; m <= 12; ++m) {
      const uint32_t last = kLengths[m - 1] + (leap && m == 2 ? 1u : 0u);
      for (uint32_t d = 1; d <= last; ++d) {
        if (y == 2023 && m == 1 && d == 1) continue;
        const int32_t current = companion::daysFromCivil(y, m, d);
        EXPECT_EQ(current - previous, 1) << y << "-" << m << "-" << d;
        previous = current;
      }
    }
  }
}

TEST(CompanionCalendar, LeapDayIsHandled) {
  EXPECT_EQ(companion::daysBetween(2024, 2, 28, 2024, 3, 1), 2);  // 2024 is a leap year
  EXPECT_EQ(companion::daysBetween(2023, 2, 28, 2023, 3, 1), 1);
  EXPECT_EQ(companion::daysBetween(2100, 2, 28, 2100, 3, 1), 1);  // century, not a leap year
}

TEST(CompanionCalendar, SpansMonthAndYearBoundaries) {
  EXPECT_EQ(companion::daysBetween(2025, 12, 31, 2026, 1, 1), 1);
  EXPECT_EQ(companion::daysBetween(2026, 1, 31, 2026, 2, 1), 1);
  EXPECT_EQ(companion::daysBetween(2026, 1, 1, 2027, 1, 1), 365);
}

TEST(CompanionCalendar, BackwardsClockYieldsNegative) {
  // Caller treats a negative span as "the clock moved backwards", not as neglect.
  EXPECT_LT(companion::daysBetween(2026, 8, 18, 2026, 8, 17), 0);
}

// ------------------------------------------------------------ local day key

TEST(CompanionLocalDay, MatchesUtcAtZeroOffset) {
  EXPECT_EQ(companion::localDayNumber(1970, 1, 1, 0, 0, 0), 0);
  EXPECT_EQ(companion::localDayNumber(2026, 8, 18, 12, 0, 0), companion::daysFromCivil(2026, 8, 18));
}

TEST(CompanionLocalDay, EveningWestOfGreenwichStaysOnTheSameLocalDay) {
  // 02:30 UTC on the 19th is 21:30 on the 18th in UTC-5. A reading session then
  // must still count as the 18th, or the streak breaks at the wrong moment.
  const int32_t offsetMinus5 = -20;  // quarter-hours
  EXPECT_EQ(companion::localDayNumber(2026, 8, 19, 2, 30, offsetMinus5), companion::daysFromCivil(2026, 8, 18));
}

TEST(CompanionLocalDay, MorningEastOfGreenwichRollsForward) {
  // 22:00 UTC on the 18th is 07:00 on the 19th in UTC+9.
  const int32_t offsetPlus9 = 36;
  EXPECT_EQ(companion::localDayNumber(2026, 8, 18, 22, 0, offsetPlus9), companion::daysFromCivil(2026, 8, 19));
}

TEST(CompanionLocalDay, HandlesExtremeOffsets) {
  const int32_t offsetPlus14 = 56;
  const int32_t offsetMinus12 = -48;
  EXPECT_EQ(companion::localDayNumber(2026, 8, 18, 12, 0, offsetPlus14), companion::daysFromCivil(2026, 8, 19));
  EXPECT_EQ(companion::localDayNumber(2026, 8, 18, 6, 0, offsetMinus12), companion::daysFromCivil(2026, 8, 17));
}

TEST(CompanionLocalDay, QuarterHourOffsetsWork) {
  // Nepal is UTC+05:45.
  const int32_t offsetPlus545 = 23;
  EXPECT_EQ(companion::localDayNumber(2026, 8, 18, 18, 20, offsetPlus545), companion::daysFromCivil(2026, 8, 19));
  EXPECT_EQ(companion::localDayNumber(2026, 8, 18, 18, 10, offsetPlus545), companion::daysFromCivil(2026, 8, 18));
}

TEST(CompanionLocalDay, ConsecutiveLocalDaysDifferByOne) {
  const int32_t offset = -20;
  const int32_t d1 = companion::localDayNumber(2026, 12, 31, 20, 0, offset);
  const int32_t d2 = companion::localDayNumber(2027, 1, 1, 20, 0, offset);
  EXPECT_EQ(d2 - d1, 1);
}

// ------------------------------------------------------------- day ledger

using companion::DayLedger;

namespace {
constexpr uint16_t kContent = 2;
constexpr int32_t kDay = 20000;  // arbitrary local day number
}  // namespace

TEST(CompanionLedger, FirstQualifyingDayStartsAStreak) {
  DayLedger led;
  companion::creditDay(led, kDay, 30, kContent);
  EXPECT_EQ(led.minutesToday, 30);
  EXPECT_EQ(led.streakDays, 1);
  EXPECT_EQ(led.bestStreakDays, 1);
  EXPECT_EQ(led.lastQualifyingDay, kDay);
}

TEST(CompanionLedger, MinutesAccumulateWithinOneDay) {
  DayLedger led;
  companion::creditDay(led, kDay, 10, kContent);
  companion::creditDay(led, kDay, 15, kContent);
  EXPECT_EQ(led.minutesToday, 25);
  EXPECT_EQ(led.streakDays, 1);  // still one day, not two
}

TEST(CompanionLedger, ConsecutiveDaysExtendTheStreak) {
  DayLedger led;
  for (int32_t d = 0; d < 5; ++d) companion::creditDay(led, kDay + d, 30, kContent);
  EXPECT_EQ(led.streakDays, 5);
  EXPECT_EQ(led.bestStreakDays, 5);
  EXPECT_EQ(led.minutesToday, 30);  // counter reset each new day
}

TEST(CompanionLedger, GapResetsStreakButKeepsBest) {
  DayLedger led;
  for (int32_t d = 0; d < 4; ++d) companion::creditDay(led, kDay + d, 30, kContent);
  ASSERT_EQ(led.streakDays, 4);
  companion::creditDay(led, kDay + 10, 30, kContent);  // week-long gap
  EXPECT_EQ(led.streakDays, 1);
  EXPECT_EQ(led.bestStreakDays, 4);
}

TEST(CompanionLedger, TrivialReadingDoesNotQualifyTheDay) {
  DayLedger led;
  companion::creditDay(led, kDay, 1, kContent);  // under contentMinutes
  EXPECT_EQ(led.minutesToday, 1);
  EXPECT_EQ(led.streakDays, 0);
  EXPECT_EQ(led.lastQualifyingDay, DayLedger::NEVER);
}

TEST(CompanionLedger, TrivialThenRealReadingQualifiesOnce) {
  DayLedger led;
  companion::creditDay(led, kDay, 1, kContent);
  companion::creditDay(led, kDay, 1, kContent);  // now at 2, crosses the bar
  EXPECT_EQ(led.streakDays, 1);
  companion::creditDay(led, kDay, 50, kContent);  // more of the same day
  EXPECT_EQ(led.streakDays, 1);                   // must not double-count
}

TEST(CompanionLedger, BackwardsClockStartsAFreshDayCounter) {
  DayLedger led;
  companion::creditDay(led, kDay, 40, kContent);
  companion::creditDay(led, kDay - 3, 10, kContent);  // clock corrected backwards
  EXPECT_EQ(led.counterDay, kDay - 3);
  EXPECT_EQ(led.minutesToday, 10);
  EXPECT_EQ(led.bestStreakDays, 1);
}

TEST(CompanionLedger, MinutesSaturateInsteadOfWrapping) {
  DayLedger led;
  companion::creditDay(led, kDay, 60000, kContent);
  companion::creditDay(led, kDay, 60000, kContent);
  EXPECT_EQ(led.minutesToday, UINT16_MAX);
  EXPECT_EQ(led.streakDays, 1);  // one day, however many minutes
}

TEST(CompanionLedger, MoodInputReportsTodaysMinutes) {
  DayLedger led;
  companion::creditDay(led, kDay, 45, kContent);
  const auto in = companion::moodInputFor(led, kDay, true, 0);
  EXPECT_EQ(in.creditedMinutesToday, 45);
  EXPECT_EQ(in.daysSinceLastRead, 0);
  EXPECT_EQ(companion::evaluate(in), companion::Mood::Thriving);
}

TEST(CompanionLedger, MoodInputZeroesMinutesOnANewDay) {
  DayLedger led;
  companion::creditDay(led, kDay, 45, kContent);
  const auto in = companion::moodInputFor(led, kDay + 1, true, 0);
  EXPECT_EQ(in.creditedMinutesToday, 0);  // yesterday's minutes do not carry
  EXPECT_EQ(in.daysSinceLastRead, 1);
  EXPECT_EQ(companion::evaluate(in), companion::Mood::Content);
}

TEST(CompanionLedger, MoodDecaysAcrossQuietDays) {
  DayLedger led;
  companion::creditDay(led, kDay, 45, kContent);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 2, true, 0)), companion::Mood::Peckish);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 3, true, 0)), companion::Mood::Neglected);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 60, true, 0)), companion::Mood::Neglected);
}

TEST(CompanionLedger, FreshCompanionIsContentNotNeglected) {
  // Nothing has been read yet, so there is nothing to have neglected.
  const DayLedger led;
  const auto in = companion::moodInputFor(led, kDay, true, 0);
  EXPECT_EQ(in.daysSinceLastRead, 0);
  EXPECT_EQ(companion::evaluate(in), companion::Mood::Content);
}

TEST(CompanionLedger, BackwardsClockDoesNotReadAsNeglect) {
  DayLedger led;
  companion::creditDay(led, kDay, 45, kContent);
  const auto in = companion::moodInputFor(led, kDay - 5, true, 0);
  EXPECT_EQ(in.daysSinceLastRead, 0);
  EXPECT_NE(companion::evaluate(in), companion::Mood::Neglected);
}

TEST(CompanionLedger, ClocklessModeUsesSessionMinutesOnly) {
  DayLedger led;
  companion::creditDay(led, kDay, 45, kContent);
  const auto idle = companion::moodInputFor(led, kDay + 99, false, 0);
  EXPECT_FALSE(idle.clockValid);
  EXPECT_EQ(companion::evaluate(idle), companion::Mood::Content);

  const auto active = companion::moodInputFor(led, kDay + 99, false, 50);
  EXPECT_EQ(active.creditedMinutesToday, 50);
  EXPECT_EQ(companion::evaluate(active), companion::Mood::Thriving);
}

// ------------------------------------------------------------ reachability
// Every tier must be reachable by a plausible sequence of real behaviour, and
// each must be exited again. A mood nobody can reach is dead art.

TEST(CompanionReachability, EveryMoodOccursOverALivedTimeline) {
  DayLedger led;
  std::set<Mood> seen;
  const auto observe = [&](int32_t day) { seen.insert(companion::evaluate(companion::moodInputFor(led, day, true, 0))); };

  int32_t day = kDay;
  // Two solid days of reading.
  companion::creditDay(led, day, 45, kContent);
  observe(day);      // long session today
  observe(day + 1);  // yesterday's grace
  companion::creditDay(led, day + 1, 10, kContent);
  observe(day + 1);

  // Then it goes quiet.
  observe(day + 2);  // one day after the last qualifying day
  observe(day + 3);  // a full day skipped
  observe(day + 4);  // and another
  observe(day + 9);  // long gone

  EXPECT_EQ(seen.count(Mood::Thriving), 1u) << "Thriving unreachable";
  EXPECT_EQ(seen.count(Mood::Content), 1u) << "Content unreachable";
  EXPECT_EQ(seen.count(Mood::Peckish), 1u) << "Peckish unreachable";
  EXPECT_EQ(seen.count(Mood::Neglected), 1u) << "Neglected unreachable";
  EXPECT_EQ(seen.size(), 4u);
}

TEST(CompanionReachability, PeckishIsNotSkippedOnTheWayDown) {
  // The narrowest tier: it exists for exactly one day, between the grace day
  // and the neglected floor. An off-by-one anywhere would step straight past it.
  DayLedger led;
  companion::creditDay(led, kDay, 45, kContent);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 0, true, 0)), Mood::Thriving);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 1, true, 0)), Mood::Content);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 2, true, 0)), Mood::Peckish);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay + 3, true, 0)), Mood::Neglected);
}

TEST(CompanionReachability, EveryTierIsExitableBackToTheTop) {
  // Recovery must work from the floor, with no penalty box. Starts at one quiet
  // day: with zero the companion is already Thriving, so there is nothing to
  // recover from.
  for (int32_t quietDays : {1, 2, 3, 50, 5000}) {
    DayLedger led;
    companion::creditDay(led, kDay, 45, kContent);
    const int32_t today = kDay + quietDays;
    ASSERT_NE(companion::evaluate(companion::moodInputFor(led, today, true, 0)), Mood::Thriving) << quietDays;

    companion::creditDay(led, today, 40, kContent);
    EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, today, true, 0)), Mood::Thriving)
        << "could not recover after " << quietDays << " quiet days";
  }
}

TEST(CompanionReachability, ThresholdBoundariesAreExact) {
  const MoodThresholds t;
  ASSERT_GT(t.thrivingMinutes, t.contentMinutes) << "Thriving must sit above Content or a tier is unreachable";

  DayLedger led;
  companion::creditDay(led, kDay, t.thrivingMinutes - 1, kContent);
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay, true, 0)), Mood::Content)
      << "one minute short of Thriving";
  companion::creditDay(led, kDay, 1, kContent);  // exactly on the threshold
  EXPECT_EQ(companion::evaluate(companion::moodInputFor(led, kDay, true, 0)), Mood::Thriving) << "exactly Thriving";

  DayLedger low;
  companion::creditDay(low, kDay, t.contentMinutes - 1, kContent);
  EXPECT_EQ(low.lastQualifyingDay, DayLedger::NEVER) << "below contentMinutes must not qualify the day";
  companion::creditDay(low, kDay, 1, kContent);  // exactly on the threshold
  EXPECT_EQ(low.lastQualifyingDay, kDay) << "exactly contentMinutes must qualify the day";
}

TEST(CompanionReachability, EveryMoodHasArtAndIsIndexable) {
  // The enum is used to index the generated sprite and quote tables, so the
  // values must stay contiguous from zero with no gaps.
  EXPECT_EQ(static_cast<int>(Mood::Thriving), 0);
  EXPECT_EQ(static_cast<int>(Mood::Content), 1);
  EXPECT_EQ(static_cast<int>(Mood::Peckish), 2);
  EXPECT_EQ(static_cast<int>(Mood::Neglected), 3);
}

// ------------------------------------------------------- session accumulator

TEST(CompanionSession, CreditsNothingWithoutAPageTurn) {
  SessionAccumulator acc;
  acc.onTick(0);
  acc.onTick(120);
  acc.onTick(240);
  EXPECT_EQ(acc.creditedSeconds(), 0u);
}

TEST(CompanionSession, CreditsTimeAfterAPageTurn) {
  SessionAccumulator acc;
  acc.onPageTurn(0);
  acc.onTick(60);
  EXPECT_EQ(acc.creditedSeconds(), 60u);
}

TEST(CompanionSession, StopsCreditingOnceTheWindowLapses) {
  SessionAccumulator acc(300);
  acc.onPageTurn(0);
  acc.onTick(300);  // still inside the window
  EXPECT_EQ(acc.creditedSeconds(), 300u);

  acc.onTick(600);  // 600s since the last page turn: idle, credit nothing
  EXPECT_EQ(acc.creditedSeconds(), 300u);
  acc.onTick(900);
  EXPECT_EQ(acc.creditedSeconds(), 300u);
}

TEST(CompanionSession, ResumesCreditingAfterANewPageTurn) {
  SessionAccumulator acc(300);
  acc.onPageTurn(0);
  acc.onTick(600);   // lapsed; nothing banked
  acc.onPageTurn(600);
  acc.onTick(660);
  EXPECT_EQ(acc.creditedSeconds(), 60u);
}

TEST(CompanionSession, LongStallIsCappedAtTheWindow) {
  // A starved loop must not bank an hour of "reading" in one tick.
  SessionAccumulator acc(300);
  acc.onPageTurn(0);
  acc.onPageTurn(3600);  // keeps the session active across the stall
  acc.onTick(3601);
  EXPECT_LE(acc.creditedSeconds(), 300u);
}

TEST(CompanionSession, BackwardsClockBanksNothingAndReAnchors) {
  SessionAccumulator acc;
  acc.onPageTurn(1000);
  acc.onTick(1060);
  EXPECT_EQ(acc.creditedSeconds(), 60u);

  acc.onTick(500);  // clock moved backwards
  EXPECT_EQ(acc.creditedSeconds(), 60u);

  acc.onPageTurn(500);
  acc.onTick(530);  // re-anchored, so this interval is sane again
  EXPECT_EQ(acc.creditedSeconds(), 90u);
}

TEST(CompanionSession, IdleReaderEarnsNothingOverALongSitting) {
  // The core anti-gaming property: device open on one page all afternoon.
  SessionAccumulator acc(300);
  acc.onPageTurn(0);
  for (uint32_t t = 30; t <= 7200; t += 30) acc.onTick(t);
  EXPECT_LE(acc.creditedMinutes(), 5u);
}

TEST(CompanionSession, SustainedReadingAccruesRealMinutes) {
  // Turning a page every half minute for an hour is genuine reading.
  SessionAccumulator acc(300);
  for (uint32_t t = 0; t <= 3600; t += 30) {
    acc.onPageTurn(t);
    acc.onTick(t);
  }
  EXPECT_EQ(acc.creditedMinutes(), 60u);
}

TEST(CompanionSession, ResetClearsEverything) {
  SessionAccumulator acc;
  acc.onPageTurn(0);
  acc.onTick(120);
  ASSERT_GT(acc.creditedSeconds(), 0u);
  acc.reset();
  EXPECT_EQ(acc.creditedSeconds(), 0u);
  acc.onTick(1000);  // no page turn since reset
  EXPECT_EQ(acc.creditedSeconds(), 0u);
}
