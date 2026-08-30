#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  mutable std::mutex _mutex;
  CrossPointState() = default;
  friend class PersistableStore<CrossPointState>;

 public:
  // Access the state mutex for protecting multi-field reads/writes from other cores.
  std::mutex& getMutex() const { return _mutex; }

  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  std::string favoriteSleepImagePath;
  std::string preferredSleepFolderPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // One-shot marker set only when Quick Lock puts the device to sleep.
  bool quickLockResumePending = false;
  // Serialized raw QuickLockTrigger value for the one permitted post-wake unlock.
  uint8_t quickLockResumeTrigger = 0;

  // AO3 library: selector index to restore when returning to the library screen.
  // Not persisted; -1 means no return index is pending.
  int ao3LibraryReturnIndex = -1;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void clearRecentSleepHistory();
  ~CrossPointState() = default;

  bool saveToFile() const;

  bool loadFromFile();
  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  uint16_t pendingBookmarkSpine = UINT16_MAX;
  float pendingBookmarkProgress = -1.0f;
  uint16_t pendingBookmarkParagraphIndex = UINT16_MAX;
  uint16_t pendingClippingIndex = UINT16_MAX;
  // One-shot jump set when applying a synced BookFusion position, consumed the same way as
  // the pendingBookmark* fields above -- lets the reader's existing pendingSpineProgress/
  // pendingPercentJump machinery land on the correct page once the target chapter's real page
  // boundaries are known, instead of always landing on page 0 of the chapter.
  uint16_t pendingBookFusionSyncSpine = UINT16_MAX;
  float pendingBookFusionSyncProgress = -1.0f;
  // 0 on a fresh sync; set to 1 when the reader re-arms this same jump for one retry with a
  // clean heap after a low-memory silent restart. Caps the retry at once so a chapter that's
  // structurally too big to lay out (not just fragmented) degrades to chapter start instead of
  // rebooting forever.
  uint8_t pendingBookFusionSyncRetryCount = 0;

  // Set by background move task on failure; read and cleared by ActivityManager to show AlertActivity.
  // Title/body are written before the flag is set to ensure they are visible when flag is read.
  std::atomic<bool> hasPendingAlert{false};
  std::atomic<bool> pendingAlertGoHomeOnBack{false};
  char pendingAlertTitle[64] = {};
  char pendingAlertBody[256] = {};

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
