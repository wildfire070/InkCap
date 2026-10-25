#include "BookFusionSyncActivity.h"

#include <DateFormatting.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "HalClock.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace {
// Same-progress tolerance used to skip the Apply/Upload prompt when both
// sides already agree.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

// BookFusion's reading-time endpoint has no server-side "now", so this is the one timestamp
// this client has to generate rather than echo back. NTP-syncs the system clock and formats
// it as UTC ISO-8601, or returns false if the result still doesn't look like a real current
// date (no RTC, no WiFi, or NTP failed) -- callers skip the push rather than send a bogus
// timestamp. See utcIsoFromEpoch() below for the other device-generated-timestamp case
// (a highlight's own creation time, already captured on-device, just needs formatting).
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

// Formats a clipping's own creationTimeUtc() (ClippingStore.cpp) as UTC ISO-8601 for the
// highlight push's added_at field. Returns false (buf untouched) for epochSeconds == 0,
// ClippingStore's own "no clock was available when this was created" sentinel.
bool utcIsoFromEpoch(const uint32_t epochSeconds, char* buf, const size_t bufSize) {
  if (epochSeconds == 0) return false;
  const time_t t = static_cast<time_t>(epochSeconds);
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);
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
  // EpubReaderActivity loads CLIPPINGS in its own onEnter() and unloads it in its own
  // onExit() -- replacing it with this activity (see the BOOKFUSION_SYNC case in
  // EpubReaderActivity.cpp) runs that unload() before this activity's onEnter() ever sees the
  // book, so without this, pushHighlights() always finds the store empty and silently pushes
  // nothing, no matter how many highlights the book actually has.
  CLIPPINGS.loadForBook(epubPath, epub->getTitle(), epub->getAuthor(), "epub");
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
  // Mirrors EpubReaderActivity's own load-in-onEnter/unload-in-onExit discipline for this same
  // global store (see ensureEpubLoaded()) -- returning to the reader reloads it there
  // regardless, but this keeps the store from being left pointed at a book that's no longer
  // active if the user instead backs out to Home from a failed/finished sync.
  if (epub) {
    CLIPPINGS.unload();
  }
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

namespace {
// Bounds the heap/network time this adds to a sync; the rest stay pending for next time.
constexpr int MAX_HIGHLIGHT_PUSH_PER_SYNC = 8;

// Formats a server "YYYY-MM-DD..." timestamp (BookFusionSyncBaseline::syncedAtUtcIso) using
// the user's own date format/separator settings. Returns false (buf untouched) for anything
// that doesn't start with a parseable date, same as a missing baseline.
bool formatLastSyncedDate(const std::string& iso, char* buf, size_t bufSize) {
  unsigned year = 0, month = 0, day = 0;
  if (iso.size() < 10 || sscanf(iso.c_str(), "%4u-%2u-%2u", &year, &month, &day) != 3) return false;
  const char separator = SETTINGS.dateSeparator == CrossPointSettings::DATE_SEPARATOR_PERIOD  ? '.'
                         : SETTINGS.dateSeparator == CrossPointSettings::DATE_SEPARATOR_HYPHEN ? '-'
                                                                                               : '/';
  return formatDateParts(buf, bufSize, static_cast<uint16_t>(year), static_cast<uint8_t>(month),
                         static_cast<uint8_t>(day), SETTINGS.dateFormat, separator);
}
}  // namespace

// Highlights only, deliberately: BookFusion's real API (confirmed by reading BookFusion's own
// official KOReader plugin source, since there's no public documentation) has no concept of a
// plain, text-less bookmark -- only a highlighted passage with real quoted text can be pushed.
// An earlier version of this function also tried to push BookmarkStore entries to a
// /api/v2/library/.../bookmarks endpoint that turned out not to be the real one at all (every
// create attempt failed server-side); dropped rather than fixed, since there's nothing on the
// server side for a text-less bookmark to become.
void BookFusionSyncActivity::pushHighlights() {
  if (!epub) return;
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return;

  int pushed = 0;

  const auto& clippings = CLIPPINGS.getClippings();
  for (size_t i = 0; i < clippings.size() && pushed < MAX_HIGHLIGHT_PUSH_PER_SYNC; i++) {
    const Clipping& clipping = clippings[i];
    if (clipping.bookFusionId != 0 || clipping.spineIndex >= spineCount || clipping.pageCount == 0) continue;

    std::string text;
    if (!CLIPPINGS.readClippingText(i, text) || text.empty()) continue;

    BookFusionHighlight highlight;
    highlight.bookId = bookId;
    highlight.chapterIndex = clipping.spineIndex;
    highlight.quoteText = text;
    highlight.chapterTitle = clipping.chapterTitle;
    char addedAtBuf[24];
    if (utcIsoFromEpoch(clipping.timestamp, addedAtBuf, sizeof(addedAtBuf))) {
      highlight.addedAt = addedAtBuf;
    }

    uint32_t remoteId = 0;
    const auto result = BookFusionSyncClient::createHighlight(highlight, &remoteId);
    if (result == BookFusionSyncClient::AUTH_FAILED || result == BookFusionSyncClient::NETWORK_ERROR) {
      LOG_INF("BFSync", "Highlight push stopped: %s", BookFusionSyncClient::errorString(result).c_str());
      return;
    }
    if (result != BookFusionSyncClient::OK || remoteId == 0) continue;  // Retried next sync.
    CLIPPINGS.setClippingFusionId(i, remoteId);
    pushed++;
  }
}

void BookFusionSyncActivity::performSync() {
  if (!ensureEpubLoaded()) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = tr(STR_BF_EPUB_LOAD_FAILED);
    requestUpdate();
    return;
  }

  // Independent of the progress comparison below: never blocks or fails this sync.
  pushHighlights();

  localHighlightCount = static_cast<int>(CLIPPINGS.getClippings().size());
  syncedHighlightCount = static_cast<int>(std::count_if(
      CLIPPINGS.getClippings().begin(), CLIPPINGS.getClippings().end(),
      [](const Clipping& clipping) { return clipping.bookFusionId != 0; }));

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
  lastSyncedAtUtcIso = baseline.hasBaseline ? baseline.syncedAtUtcIso : std::string();
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

  // Prefer the chapter index BookFusion reported, deriving the intra-chapter fraction from
  // pagePositionInBook the same way it was encoded when captured (pagePositionInBook =
  // (spineIndex + intra) / spineCount, so intra = pagePositionInBook * spineCount - spineIndex).
  // Falls back to resolving a chapter from the percentage alone -- via the same location-index
  // system used elsewhere in this codebase -- when the remote position predates chapter info.
  int spineIndex = 0;
  float intraSpineProgress = 0.0f;
  bool saved = false;
  const int spineCount = epub->getSpineItemsCount();
  if (remoteSyncProgress.hasChapterInfo && remoteSyncProgress.chapterIndex >= 0 &&
      remoteSyncProgress.chapterIndex < spineCount) {
    spineIndex = remoteSyncProgress.chapterIndex;
    if (spineCount > 0) {
      intraSpineProgress = std::max(
          0.0f, std::min(1.0f, remoteSyncProgress.pagePositionInBook * static_cast<float>(spineCount) -
                                    static_cast<float>(spineIndex)));
    }
  } else {
    const int percentInt = std::max(0, std::min(100, static_cast<int>(remotePercent * 100.0f + 0.5f)));
    if (!epub->resolveLocationPercentToSpineProgress(percentInt, spineIndex, intraSpineProgress)) {
      intraSpineProgress = -1.0f;  // signals "couldn't resolve" below, distinct from a real 0.0f
    }
  }

  if (intraSpineProgress >= 0.0f) {
    // The reader hasn't built this chapter yet, so its real page count is unknown -- save a
    // fixed-precision placeholder (e.g. intra=0.3 -> page 2999/10000) that approximates the
    // actual fraction, rather than page 0 of 1, which loadProgress()'s (page+1)/pageCount math
    // would read back as 100% through the chapter instead of "just starting it". That wrong
    // reading was making the very next sync's already-synced/conflict check always see local
    // progress as ahead of what was just applied, so it never recognized two sides as matching.
    // pendingBookFusionSyncSpine/Progress (set below) still drive the reader's actual landing
    // page once the chapter builds and overwrites this placeholder with the real value.
    constexpr int kPlaceholderPageCount = 10000;
    const int placeholderPage =
        std::max(0, std::min(kPlaceholderPageCount - 1, static_cast<int>(intraSpineProgress * kPlaceholderPageCount)));
    saved = EpubReaderUtils::saveProgress(*epub, spineIndex, placeholderPage, kPlaceholderPageCount);
  }

  if (!saved) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    errorMessage = tr(STR_SAVE_PROGRESS_FAILED);
    requestUpdate();
    return;
  }

  // Hand the intra-chapter fraction to the reader the same way a bookmark jump does --
  // EpubReaderActivity::onEnter() resolves it against the target chapter's real page count
  // once that section actually builds, landing closer to the synced position than page 0.
  APP_STATE.pendingBookFusionSyncSpine = static_cast<uint16_t>(spineIndex);
  APP_STATE.pendingBookFusionSyncProgress = intraSpineProgress;
  APP_STATE.saveToFile();

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
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        TouchHeaderBackButton::wasTapped(mappedInput, header)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || TouchHeaderBackButton::wasTapped(mappedInput, header)) {
      returnToReader();
      return;
    }
    // Tap either choice row directly -- matches the tap-to-choose convention OptionPopup/
    // TouchActionButtons use elsewhere (no separate select-then-confirm step for touch).
    // Rows are registered in TouchRegistry by the render() call that just drew them, same as
    // every other touch row in this codebase (TouchRegistry::getInstance().add() + here,
    // wasItemTapped()) -- this used to hand-compute a y-band and compare tapY directly, which
    // is the hand-rolled-row-geometry pattern that's repeatedly proven unreliable on real
    // hardware even when it looks correct in review/emulation.
    int tappedItem = -1;
    if (mappedInput.wasItemTapped(tappedItem) && (tappedItem == 0 || tappedItem == 1)) {
      mappedInput.suppressCurrentTouchContact();
      selectedOption = tappedItem;
      if (tappedItem == 0) {
        applyRemoteProgress();
      } else {
        uploadLocalProgress();
      }
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

  const auto pageHeight = renderer.getScreenHeight();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BF_SYNC), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BF_SYNC));
  }

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineH) / 2;

  switch (state) {
    case CONNECTING:
    case SYNCING:
    case UPLOADING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
      break;
    case SHOWING_RESULT: {
      // Variable line count (bookmark/highlight/last-synced lines only appear when there's
      // something to say), so the whole block is built first and centred as one unit rather
      // than hand-placing each line at a fixed offset from `top`.
      struct ResultLine {
        std::string text;
        bool bold;
        int optionIndex = -1;  // 0/1 for the Apply Remote/Upload Local choice rows, -1 otherwise
      };
      std::vector<ResultLine> lines;

      char localBuf[48];
      snprintf(localBuf, sizeof(localBuf), tr(STR_BF_LOCAL_PROGRESS_FORMAT), localPercent * 100.0f);
      if (localPercent > remotePercent + SAME_PROGRESS_EPSILON) strncat(localBuf, tr(STR_BF_AHEAD_SUFFIX), 15);
      lines.push_back({localBuf, false});

      char remoteBuf[48];
      snprintf(remoteBuf, sizeof(remoteBuf), tr(STR_BF_REMOTE_PROGRESS_FORMAT), remotePercent * 100.0f);
      if (remotePercent > localPercent + SAME_PROGRESS_EPSILON) strncat(remoteBuf, tr(STR_BF_AHEAD_SUFFIX), 15);
      lines.push_back({remoteBuf, false});

      char countBuf[48];
      if (localHighlightCount > 0) {
        snprintf(countBuf, sizeof(countBuf), tr(STR_BF_HIGHLIGHTS_SYNCED_FORMAT), syncedHighlightCount,
                 localHighlightCount);
        lines.push_back({countBuf, false});
      }

      char dateBuf[24];
      if (!lastSyncedAtUtcIso.empty() && formatLastSyncedDate(lastSyncedAtUtcIso, dateBuf, sizeof(dateBuf))) {
        char lastSyncedBuf[48];
        snprintf(lastSyncedBuf, sizeof(lastSyncedBuf), tr(STR_BF_LAST_SYNCED_FORMAT), dateBuf);
        lines.push_back({lastSyncedBuf, false});
      }

      // Both choices shown as their own line (rather than one line that cycles), so touch
      // devices have something to tap directly -- see resultOptionY below. The selected one
      // is bold, matching how Left/Right already highlighted it on button devices.
      lines.push_back({tr(STR_BF_APPLY_REMOTE), selectedOption == 0, 0});
      lines.push_back({tr(STR_BF_UPLOAD_LOCAL), selectedOption == 1, 1});

      constexpr int LINE_GAP = 8;
      const int blockHeight = static_cast<int>(lines.size()) * lineH + (static_cast<int>(lines.size()) - 1) * LINE_GAP;
      const int pageWidth = renderer.getScreenWidth();
      int y = (pageHeight - blockHeight) / 2;
      for (const auto& line : lines) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, line.text.c_str(), true,
                                  line.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        // Register each choice row's full-width tap zone -- rendering is the only place that
        // knows the real layout (lines.size() varies with which counts are shown).
        if (line.optionIndex >= 0) {
          TouchRegistry::getInstance().add(Rect{0, y, pageWidth, lineH}, line.optionIndex, TouchRegistry::Item);
        }
        y += lineH + LINE_GAP;
      }
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
