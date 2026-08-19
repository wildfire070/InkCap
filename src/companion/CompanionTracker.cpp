#include "CompanionTracker.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>

#include "CompanionState.h"
#include "CrossPointSettings.h"

namespace {
// Persist part-way through a long session so a flat battery or a hard power-off
// does not discard an evening's reading. Chosen well above the SD write cost
// and well below a session worth losing.
constexpr uint32_t BANK_INTERVAL_S = 300;

// clockUtcOffsetQ is biased by 48 so it fits in a uint8_t (48 == UTC+0).
int32_t signedUtcOffsetQuarterHours() {
  uint8_t biased = SETTINGS.clockUtcOffsetQ;
  if (biased > 104) biased = 104;  // guard a corrupted persisted value
  return static_cast<int32_t>(biased) - 48;
}
}  // namespace

bool CompanionTracker::isEnabled() { return SETTINGS.companionEnabled != 0; }

companion::CompanionId CompanionTracker::activeId() {
  const uint8_t id = SETTINGS.companionId;
  if (id >= companion::COMPANION_COUNT) return static_cast<companion::CompanionId>(0);
  return static_cast<companion::CompanionId>(id);
}

void CompanionTracker::refreshDay() {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;

  if (!halClock.getUtcDateTime(year, month, day, hour, minute)) {
    clockValid = false;
    return;
  }
  clockValid = true;
  localDay = companion::localDayNumber(year, month, day, hour, minute, signedUtcOffsetQuarterHours());
}

void CompanionTracker::beginSession() {
  if (!isEnabled()) return;

  accumulator.reset();
  pagesThisSession = 0;
  bankedSeconds = 0;
  sessionActive = true;
  refreshDay();
}

void CompanionTracker::refreshForDisplay() {
  if (!isEnabled()) return;
  // Outside a session there is no accumulator to disturb; only the cached day
  // and clock validity are updated so currentMood() reflects real elapsed days.
  if (!sessionActive) refreshDay();
}

void CompanionTracker::onPageTurn() {
  if (!isEnabled() || !sessionActive) return;
  accumulator.onPageTurn(millis() / 1000);
  pagesThisSession++;
}

void CompanionTracker::tick() {
  if (!isEnabled() || !sessionActive) return;
  accumulator.onTick(millis() / 1000);

  // Mid-session checkpoint. Only credited time counts, so an idle reader never
  // triggers an SD write.
  if (accumulator.creditedSeconds() - bankedSeconds >= BANK_INTERVAL_S) {
    if (bankSession()) COMPANION_STATE.saveToFile();
  }
}

bool CompanionTracker::bankSession() {
  const uint32_t unbanked = accumulator.creditedSeconds() - bankedSeconds;
  if (unbanked == 0 && pagesThisSession == 0) return false;

  const bool changed = COMPANION_STATE.recordSession(unbanked, pagesThisSession, clockValid, localDay);
  bankedSeconds = accumulator.creditedSeconds();
  pagesThisSession = 0;
  return changed;
}

void CompanionTracker::endSession() {
  if (!isEnabled() || !sessionActive) return;

  accumulator.onTick(millis() / 1000);
  // The day can have rolled over mid-session (reading past midnight), so
  // re-resolve it before attributing the remaining minutes.
  refreshDay();

  if (bankSession() && !COMPANION_STATE.saveToFile()) {
    LOG_ERR("COMP", "Failed to save companion state");
  }
  sessionActive = false;
}

companion::MoodInput CompanionTracker::buildMoodInput() const {
  companion::MoodInput in =
      companion::moodInputFor(COMPANION_STATE.ledger, localDay, clockValid, accumulator.creditedMinutes());

  if (clockValid) {
    // The ledger only holds minutes banked at the last checkpoint, so add the
    // unbanked remainder: the mood should react during a long first session,
    // not only after it crosses a checkpoint. The clockless branch already
    // reports the whole session, so adding it there would double-count.
    const uint32_t unbanked = (accumulator.creditedSeconds() - bankedSeconds) / 60;
    const uint32_t total = in.creditedMinutesToday + unbanked;
    in.creditedMinutesToday = total > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(total);
  }
  return in;
}

companion::Mood CompanionTracker::currentMood() const { return companion::evaluate(buildMoodInput()); }

uint16_t CompanionTracker::minutesToday() const { return buildMoodInput().creditedMinutesToday; }
