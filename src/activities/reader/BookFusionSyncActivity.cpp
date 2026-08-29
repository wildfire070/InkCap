#include "BookFusionSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cmath>
#include <ctime>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "HalClock.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace {
// Same-progress tolerance used to skip the Apply/Upload prompt when both
// sides already agree.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

// Every other timestamp this client ever sends is server-reported -- this is
// the one exception (BookFusion's reading-time endpoint has no server-side
// "now"). NTP-syncs the system clock and formats it as UTC ISO-8601, or
// returns false if the result still doesn't look like a real current date
// (no RTC, no WiFi, or NTP failed) -- callers skip the push rather than
// send a bogus timestamp.
bool currentUtcIsoTimestamp(char* buf, size_t bufSize) {
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_DBG("BFSync", "NTP sync unavailable for reading-time timestamp");
  }
#endif
  const time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  if (timeinfo.tm_year + 1900 < 2024) return false;  // Clock still looks unsynced.
  strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return true;
}

// Pushes the reading-time delta since the last successful push, if any.
// Best-effort: failure here doesn't fail the overall progress push.
void pushReadingTimeDelta(uint32_t bookId, const Epub& epub, const std::string& epubPath) {
  const auto stats = BookReadingStats::load(epub.getCachePath());
  if (stats.totalReadingSeconds == 0) return;

  const uint32_t syncedSeconds = BookFusionBookIdStore::loadSyncedReadingSeconds(epubPath);
  if (stats.totalReadingSeconds <= syncedSeconds) return;
  const uint32_t deltaSeconds = stats.totalReadingSeconds - syncedSeconds;
  if (deltaSeconds < 5) return;  // Below BookFusion's minimum.

  char loggedAt[32];
  if (!currentUtcIsoTimestamp(loggedAt, sizeof(loggedAt))) return;

  const auto result = BookFusionSyncClient::trackReadingTime(bookId, deltaSeconds, loggedAt);
  if (result == BookFusionSyncClient::OK) {
    BookFusionBookIdStore::saveSyncedReadingSeconds(epubPath, stats.totalReadingSeconds);
  } else {
    LOG_DBG("BFSync", "trackReadingTime failed: %s", BookFusionSyncClient::errorString(result).c_str());
  }
}

// Does pos match what the last sync recorded? Chapter-exact when both sides
// have chapter granularity; percentage-only fallback otherwise (an older
// sidecar, or a position that never got chapter info).
bool matchesBaseline(const BookFusionSyncBaseline& baseline, const BookFusionProgress& pos) {
  if (!baseline.hasBaseline) return false;
  if (!pos.hasChapterInfo) return std::fabs(baseline.percentage - pos.percentage) < SAME_PROGRESS_EPSILON;
  if (baseline.chapterIndex != pos.chapterIndex) return false;
  return std::fabs(baseline.pagePositionInBook - pos.pagePositionInBook) <= 0.0005f &&
         std::fabs(baseline.percentage - pos.percentage) <= 0.05f;
}

BookFusionSyncBaseline makeBaseline(const BookFusionProgress& pos, const std::string& updatedAt) {
  BookFusionSyncBaseline baseline;
  baseline.hasBaseline = true;
  baseline.syncedAtUtcIso = updatedAt;
  baseline.percentage = pos.percentage;
  baseline.pagePositionInBook = pos.pagePositionInBook;
  baseline.chapterIndex = pos.chapterIndex;
  return baseline;
}
}  // namespace

bool BookFusionSyncActivity::ensureEpubLoaded() {
  if (epub) return true;
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: no CSS needed for progress mapping.
  if (!epub->load(false, true)) {
    LOG_ERR("BFSync", "Failed to load epub for progress mapping");
    epub.reset();
    return false;
  }
  return true;
}

void BookFusionSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void BookFusionSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void BookFusionSyncActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  // TODO(BLE): disable the BLE page-turner here before the WiFi/TLS calls
  // below once InkCap has BLE support -- upstream disables Bluetooth at this
  // same point (BLE and WiFi share the radio, and the BLE stack's heap use
  // competes with the TLS handshake). No-op today: InkCap has no BLE code.

  bookId = BookFusionBookIdStore::loadBookId(epubPath);
  if (bookId == 0) {
    state = NO_BOOK_LINK;
    errorMessage = tr(STR_BF_NOT_LINKED_MSG);
    requestUpdate();
    return;
  }
  if (BookFusionSyncClient::getBearerToken().empty()) {
    state = NO_BOOK_LINK;
    errorMessage = tr(STR_BF_NO_TOKEN_MSG);
    requestUpdate();
    return;
  }

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionSyncActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

void BookFusionSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_BF_SYNCING);
    powerDownAfterRender = true;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFSync", "Syncing screen could not be rendered before request");
    requestUpdate(true);
  }

  performSync();
}

void BookFusionSyncActivity::performSync() {
  if (!ensureEpubLoaded()) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = tr(STR_BF_EPUB_LOAD_FAILED);
    requestUpdate();
    return;
  }

  localSyncProgress = BookFusionProgress{};
  localSyncProgress.bookId = bookId;
  EpubReaderUtils::Progress localProgress;
  const int spineCount = epub->getSpineItemsCount();
  if (EpubReaderUtils::loadProgress(*epub, localProgress, "BFSync") && localProgress.hasPageCount &&
      localProgress.pageCount > 0) {
    const float chapterProgress =
        static_cast<float>(localProgress.pageNumber + 1) / static_cast<float>(localProgress.pageCount);
    localPercent = epub->calculateProgress(localProgress.spineIndex, chapterProgress);
    if (spineCount > 0) {
      localSyncProgress.hasChapterInfo = true;
      localSyncProgress.chapterIndex = localProgress.spineIndex;
      localSyncProgress.pagePositionInBook =
          (static_cast<float>(localProgress.spineIndex) + chapterProgress) / static_cast<float>(spineCount);
    }
  } else {
    localPercent = 0.0f;
  }
  localSyncProgress.percentage = localPercent;

  BookFusionProgress remote;
  const auto result = BookFusionSyncClient::getProgress(bookId, remote);
  hasRemoteProgress = result == BookFusionSyncClient::OK;
  if (hasRemoteProgress) {
    remotePercent = remote.percentage;
    remoteSyncProgress = remote;
  } else if (result != BookFusionSyncClient::NOT_FOUND) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = BookFusionSyncClient::errorString(result);
    requestUpdate();
    return;
  }

  if (!hasRemoteProgress) {
    // Nothing to compare against; just upload local progress.
    uploadLocalProgress();
    return;
  }

  // Beyond the raw percentage match below, also treat this as already-synced
  // if neither side has moved since the last successful sync -- catches the
  // case where local/remote don't match each other exactly (rounding,
  // chapter-boundary drift) but both still match what was last synced.
  const auto baseline = BookFusionBookIdStore::loadSyncBaseline(epubPath);
  const bool alreadySynced = std::fabs(localPercent - remotePercent) < SAME_PROGRESS_EPSILON ||
                             (matchesBaseline(baseline, localSyncProgress) && matchesBaseline(baseline, remoteSyncProgress));
  if (alreadySynced) {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
    statusMessage = tr(STR_ALREADY_SYNCED);
    markAutoReturn();
    requestUpdate();
    return;
  }

  RenderLock lock(*this);
  state = SHOWING_RESULT;
  selectedOption = remotePercent > localPercent ? 0 : 1;
  requestUpdate();
}

void BookFusionSyncActivity::applyRemoteProgress() {
  {
    RenderLock lock(*this);
    state = UPLOADING;  // reuses the "busy" state visually; no separate APPLYING state needed
    statusMessage = tr(STR_BF_APPLYING);
  }
  requestUpdate(true);

  // Prefer the chapter index BookFusion reported (still landing at the start
  // of that chapter -- BookFusion has no intra-chapter page granularity we
  // could reconstruct exactly). Fall back to resolving a chapter from the
  // percentage alone when the remote position predates chapter info.
  int spineIndex = 0;
  bool saved = false;
  if (remoteSyncProgress.hasChapterInfo && remoteSyncProgress.chapterIndex >= 0 &&
      remoteSyncProgress.chapterIndex < epub->getSpineItemsCount()) {
    spineIndex = remoteSyncProgress.chapterIndex;
    saved = EpubReaderUtils::saveProgress(*epub, spineIndex, 0, 1);
  } else {
    const int percentInt = std::max(0, std::min(100, static_cast<int>(remotePercent * 100.0f + 0.5f)));
    float spineProgress = 0.0f;
    if (epub->resolveLocationPercentToSpineProgress(percentInt, spineIndex, spineProgress)) {
      saved = EpubReaderUtils::saveProgress(*epub, spineIndex, 0, 1);
    }
  }

  if (!saved) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = tr(STR_SAVE_PROGRESS_FAILED);
    requestUpdate();
    return;
  }

  RecentBookProgress::saveCachedEpubPercent(epub->getCachePath(), remotePercent * 100.0f);
  BookFusionBookIdStore::saveSyncBaseline(epubPath, makeBaseline(remoteSyncProgress, remoteSyncProgress.updatedAt));
  returnToReader();
}

void BookFusionSyncActivity::uploadLocalProgress() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_BF_UPLOADING);
    powerDownAfterRender = true;
  }
  requestUpdate(true);

  localSyncProgress.bookId = bookId;
  localSyncProgress.percentage = localPercent;
  const auto result = BookFusionSyncClient::updateProgress(localSyncProgress);

  if (result != BookFusionSyncClient::OK) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = BookFusionSyncClient::errorString(result);
    requestUpdate();
    return;
  }

  pushReadingTimeDelta(bookId, *epub, epubPath);
  // No server-reported updated_at for a push we just made -- the baseline's
  // syncedAtUtcIso stays empty here, matching the "no comparable timestamp"
  // fallback matchesBaseline() already takes for float-epsilon comparison.
  BookFusionBookIdStore::saveSyncBaseline(epubPath, makeBaseline(localSyncProgress, ""));

  RenderLock lock(*this);
  state = SYNC_COMPLETE;
  statusMessage = tr(STR_BF_UPLOAD_COMPLETE);
  markAutoReturn();
  requestUpdate();
}

void BookFusionSyncActivity::loop() {
  if (state == WIFI_SELECTION) return;

  if (state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
    }
    return;
  }

  if (state == NO_BOOK_LINK || state == SYNC_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = selectedOption == 0 ? 1 : 0;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        applyRemoteProgress();
      } else {
        uploadLocalProgress();
      }
    }
    return;
  }
}

void BookFusionSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  GUI.drawHeader(renderer, header, tr(STR_BF_SYNC));

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineH) / 2;

  switch (state) {
    case CONNECTING:
    case SYNCING:
    case UPLOADING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
      break;
    case SHOWING_RESULT: {
      char localBuf[32];
      char remoteBuf[32];
      snprintf(localBuf, sizeof(localBuf), tr(STR_BF_LOCAL_PROGRESS_FORMAT), localPercent * 100.0f);
      snprintf(remoteBuf, sizeof(remoteBuf), tr(STR_BF_REMOTE_PROGRESS_FORMAT), remotePercent * 100.0f);
      renderer.drawCenteredText(UI_10_FONT_ID, top - lineH - 8, localBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, top, remoteBuf);
      const char* optionText = selectedOption == 0 ? tr(STR_BF_APPLY_REMOTE) : tr(STR_BF_UPLOAD_LOCAL);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 8, optionText, true, EpdFontFamily::BOLD);
      break;
    }
    case SYNC_COMPLETE:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
      break;
    case NO_BOOK_LINK:
    case SYNC_FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, top, errorMessage.c_str());
      break;
    default:
      break;
  }

  MappedInputManager::Labels labels;
  if (state == SHOWING_RESULT) {
    labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_BF_SWITCH), "");
  } else {
    labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool powerOff = powerDownAfterRender;
  powerDownAfterRender = false;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, powerOff);
}
