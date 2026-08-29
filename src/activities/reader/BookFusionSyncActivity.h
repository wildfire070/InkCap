#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"

/**
 * Syncs reading progress with BookFusion for the current book.
 *
 * Flow: connect WiFi -> fetch remote percentage -> compare with local
 * percentage -> let the user apply the remote value or upload the local one
 * (or skip the choice entirely if only one side has progress, or if neither
 * side has moved since the last sync per BookFusionBookIdStore's baseline).
 *
 * Percentage is still the primary comparison metric shown on screen, but
 * BookFusion also carries chapter_index/page_position_in_book alongside it
 * when available; "apply" prefers the reported chapter over a percentage-
 * derived guess, still landing at that chapter's start rather than an exact
 * page since BookFusion has no intra-chapter page granularity to land on.
 * This is self-contained: it only uses the generic Epub/EpubReaderUtils
 * APIs, not anything from lib/KOReaderSync/.
 */
class BookFusionSyncActivity final : public Activity {
 public:
  explicit BookFusionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath)
      : Activity("BookFusionSync", renderer, mappedInput), epubPath(std::move(epubPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    SYNC_COMPLETE,
    SYNC_FAILED,
    NO_BOOK_LINK,
  };

  std::string epubPath;
  std::shared_ptr<Epub> epub;  // null until lazy-loaded in ensureEpubLoaded()
  uint32_t bookId = 0;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  float localPercent = 0.0f;
  float remotePercent = 0.0f;
  bool hasRemoteProgress = false;

  // Full captures (percentage + chapter/page granularity where available),
  // built alongside localPercent/remotePercent in performSync(). Used for
  // applyRemoteProgress()'s chapter-accurate landing spot and for saving the
  // post-sync baseline; localPercent/remotePercent stay the source of truth
  // for the on-screen comparison and render() display.
  BookFusionProgress localSyncProgress;
  BookFusionProgress remoteSyncProgress;

  // Selection in result screen (0 = Apply remote, 1 = Upload local)
  int selectedOption = 0;

  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Set right before a render that immediately precedes a blocking network
  // call, so render() can collapse the EPD analog rails once that paint
  // completes instead of holding them powered through the multi-second
  // wait alongside WiFi TX. One-shot: consumed and cleared by the very next
  // render() regardless of which state it was set for.
  bool powerDownAfterRender = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void applyRemoteProgress();
  void uploadLocalProgress();
  bool ensureEpubLoaded();
  void returnToReader();
  void markAutoReturn();
};
