#pragma once
#include <ArduinoJson.h>
#include <CompanionMood.h>
#include <PersistableStore.h>

#include <cstdint>

/**
 * @brief Persisted companion progress: the day ledger plus lifetime totals.
 *
 * Only earned data lives here. Which companion is active and whether the
 * feature is on at all are user preferences and live in CrossPointSettings,
 * mirroring the CrossPointSettings/CrossPointState split.
 *
 * All day-rollover and streak logic is in lib/Companion so it stays host
 * testable; this class is the JSON envelope around it.
 */
class CompanionState : public PersistableStore<CompanionState> {
  CompanionState() = default;

  friend class PersistableStore<CompanionState>;

 public:
  companion::DayLedger ledger;
  uint32_t totalMinutes = 0;  // lifetime credited minutes, shown as a stat
  uint32_t totalPages = 0;    // lifetime pages turned, shown as a stat
  // Set when a session pushes the streak past its previous best, cleared once
  // the companion has actually said something about it. Persisted so the
  // moment survives the sleep between finishing a book and next opening Home,
  // which is exactly when it is most likely to be earned.
  bool milestonePending = false;

  static const char* getFilePath() { return "/.crosspoint/companion.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Folds a finished reading session into the ledger and lifetime totals.
  // `localDay` is ignored when clockValid is false: minutes still count toward
  // lifetime totals, but they cannot be attributed to a calendar day.
  // Returns true when something changed and the caller should persist.
  bool recordSession(uint32_t creditedSeconds, uint32_t pagesTurned, bool clockValid, int32_t localDay);
};

#define COMPANION_STATE CompanionState::getInstance()
