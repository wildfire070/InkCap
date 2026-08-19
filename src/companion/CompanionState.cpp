#include "CompanionState.h"

namespace {
// Keep the JSON keys short: this file is rewritten on every session end and the
// whole document is held in RAM while serialising.
constexpr uint16_t CONTENT_MINUTES = companion::MoodThresholds{}.contentMinutes;
}  // namespace

void CompanionState::toJson(JsonDocument& doc) const {
  doc["counterDay"] = ledger.counterDay;
  doc["lastQualifyingDay"] = ledger.lastQualifyingDay;
  doc["minutesToday"] = ledger.minutesToday;
  doc["streakDays"] = ledger.streakDays;
  doc["bestStreakDays"] = ledger.bestStreakDays;
  doc["totalMinutes"] = totalMinutes;
  doc["totalPages"] = totalPages;
  doc["milestonePending"] = milestonePending;
}

bool CompanionState::fromJson(JsonVariantConst doc) {
  ledger.counterDay = doc["counterDay"] | companion::DayLedger::NEVER;
  ledger.lastQualifyingDay = doc["lastQualifyingDay"] | companion::DayLedger::NEVER;
  ledger.minutesToday = doc["minutesToday"] | static_cast<uint16_t>(0);
  ledger.streakDays = doc["streakDays"] | static_cast<uint16_t>(0);
  ledger.bestStreakDays = doc["bestStreakDays"] | static_cast<uint16_t>(0);
  totalMinutes = doc["totalMinutes"] | static_cast<uint32_t>(0);
  totalPages = doc["totalPages"] | static_cast<uint32_t>(0);
  milestonePending = doc["milestonePending"] | false;

  // A hand-edited or truncated file must not leave best below current.
  if (ledger.bestStreakDays < ledger.streakDays) ledger.bestStreakDays = ledger.streakDays;
  return true;
}

bool CompanionState::recordSession(const uint32_t creditedSeconds, const uint32_t pagesTurned, const bool clockValid,
                                   const int32_t localDay) {
  const uint32_t minutes = creditedSeconds / 60;
  if (minutes == 0 && pagesTurned == 0) return false;

  // Saturate the lifetime counters instead of wrapping to zero.
  if (UINT32_MAX - totalPages < pagesTurned) {
    totalPages = UINT32_MAX;
  } else {
    totalPages += pagesTurned;
  }
  if (UINT32_MAX - totalMinutes < minutes) {
    totalMinutes = UINT32_MAX;
  } else {
    totalMinutes += minutes;
  }

  if (clockValid && minutes > 0) {
    const uint16_t clamped = minutes > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(minutes);
    const uint16_t bestBefore = ledger.bestStreakDays;
    companion::creditDay(ledger, localDay, clamped, CONTENT_MINUTES);
    // A first-ever qualifying day sets best to 1, which is not an achievement
    // worth interrupting for; only a genuine improvement on an existing record
    // earns the milestone line.
    if (bestBefore > 1 && ledger.bestStreakDays > bestBefore) milestonePending = true;
  }
  return true;
}
