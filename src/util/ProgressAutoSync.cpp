#include "ProgressAutoSync.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "CrossPointSettings.h"
#include "HalClock.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "activities/reader/BookReadingStats.h"
#include "network/WifiUtils.h"

namespace ProgressAutoSync {

namespace {
bool armed = false;
int lastSyncedSpine = -1;  // Spine index at the last threshold crossing (or push), for dedup.
float lastSyncedPercent = -1.0f;

// Reuses only the last-connected saved network, blocking for up to ~8s --
// no scan, no fallback to other saved networks (this is opportunistic
// background sync; if the usual network isn't reachable right now, skip
// this round rather than hunting for another one). Returns true if this
// call brought the connection up itself (caller should tear it down after),
// false if WiFi was already connected (caller leaves it as found).
bool connectSilentlyIfNeeded(bool& broughtUpWifi) {
  broughtUpWifi = false;
  if (hasActiveStationWifiConnection()) return true;

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_INF("BFAuto", "Skipping auto-sync: no previously-connected WiFi network to reuse");
    return false;
  }
  const auto cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_INF("BFAuto", "Skipping auto-sync: no saved credential for last-connected network %s", lastSsid.c_str());
    return false;
  }

  LOG_INF("BFAuto", "Silently connecting to %s", cred->ssid.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());

  constexpr int POLL_INTERVAL_MS = 100;
  constexpr int MAX_POLLS = 80;  // ~8s
  for (int i = 0; i < MAX_POLLS; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      broughtUpWifi = true;
      return true;
    }
    delay(POLL_INTERVAL_MS);
  }

  LOG_INF("BFAuto", "Silent connect to %s timed out; skipping auto-sync", cred->ssid.c_str());
  WiFi.disconnect(true);
  return false;
}

void teardownWifi() {
  WiFi.disconnect(true);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

bool currentUtcIsoTimestamp(char* buf, size_t bufSize) {
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_DBG("BFAuto", "NTP sync unavailable for reading-time timestamp");
  }
#endif
  const time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  if (timeinfo.tm_year + 1900 < 2024) return false;
  strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return true;
}

void pushReadingTimeDelta(uint32_t bookId, const std::string& epubPath, const std::string& epubCachePath) {
  const auto stats = BookReadingStats::load(epubCachePath);
  if (stats.totalReadingSeconds == 0) return;
  const uint32_t syncedSeconds = BookFusionBookIdStore::loadSyncedReadingSeconds(epubPath);
  if (stats.totalReadingSeconds <= syncedSeconds) return;
  const uint32_t deltaSeconds = stats.totalReadingSeconds - syncedSeconds;
  if (deltaSeconds < 5) return;

  char loggedAt[32];
  if (!currentUtcIsoTimestamp(loggedAt, sizeof(loggedAt))) return;
  if (BookFusionSyncClient::trackReadingTime(bookId, deltaSeconds, loggedAt) == BookFusionSyncClient::OK) {
    BookFusionBookIdStore::saveSyncedReadingSeconds(epubPath, stats.totalReadingSeconds);
  }
}

void performPush(GfxRenderer& renderer, uint32_t bookId, const std::string& epubPath,
                 const std::string& epubCachePath, float bookPercent, int spineIndex, int pageNumber, int pageCount,
                 int spineCount) {
  bool broughtUpWifi = false;
  if (!connectSilentlyIfNeeded(broughtUpWifi)) return;

  // INF (not DBG) deliberately: this whole file was previously DBG-only, which is compiled
  // out at this build's LOG_LEVEL -- there was no way to tell from a capture whether a silent
  // auto-sync push (and the SD font release/restore cycle around it) ran at all.
  LOG_INF("BFAuto", "Starting silent progress push for book %lu", (unsigned long)bookId);
  sdFontSystem.releaseForNetwork(renderer);
  {
    // Nothing in this scope renders -- this whole push is silent, no screen
    // update -- so it's safe to hold the framebuffer released for both
    // calls below, matching every other BookFusion network call site.
    GfxRenderer::NetworkBufferLoan fbLoan(renderer);

    BookFusionProgress progress;
    progress.bookId = bookId;
    progress.percentage = bookPercent;
    if (spineCount > 0 && pageCount > 0) {
      const float chapterProgress = static_cast<float>(pageNumber + 1) / static_cast<float>(pageCount);
      progress.hasChapterInfo = true;
      progress.chapterIndex = spineIndex;
      progress.pagePositionInBook = (static_cast<float>(spineIndex) + chapterProgress) / static_cast<float>(spineCount);
    }

    const auto pushResult = BookFusionSyncClient::updateProgress(progress);
    if (pushResult == BookFusionSyncClient::OK) {
      BookFusionSyncBaseline baseline;
      baseline.hasBaseline = true;
      baseline.percentage = progress.percentage;
      baseline.pagePositionInBook = progress.pagePositionInBook;
      baseline.chapterIndex = progress.chapterIndex;
      BookFusionBookIdStore::saveSyncBaseline(epubPath, baseline);
      LOG_INF("BFAuto", "Pushed %.1f%% for book %lu", bookPercent * 100.0f, (unsigned long)bookId);
    } else {
      LOG_ERR("BFAuto", "Auto-sync push failed for book %lu: %s", (unsigned long)bookId,
              BookFusionSyncClient::errorString(pushResult).c_str());
    }

    pushReadingTimeDelta(bookId, epubPath, epubCachePath);
  }

  // releaseForNetwork() above freed the reader's resident SD font entirely (see its doc
  // comment: "Call ensureLoaded() later to restore it before reader rendering"). Every other
  // caller of releaseForNetwork() is a dedicated activity that either restores explicitly or
  // gets exited, naturally re-triggering EpubReaderActivity::onEnter()'s own ensureLoaded()
  // call. This push is unique in firing silently while the reader stays open and keeps
  // rendering immediately after, so without this call the font stays unloaded for the rest of
  // the session -- every glyph misses and renders as the U+FFFD replacement character.
  sdFontSystem.ensureLoaded(renderer);

  if (broughtUpWifi) teardownWifi();
}
}  // namespace

void resetSessionBaseline() {
  armed = false;
  lastSyncedSpine = -1;
  lastSyncedPercent = -1.0f;
}

void armIfThresholdCrossed(int spineIndex, float bookPercent) {
  const auto mode = SETTINGS.autosyncMode;
  if (mode == CrossPointSettings::AUTOSYNC_OFF || mode == CrossPointSettings::AUTOSYNC_ON_EXIT) return;

  if (mode == CrossPointSettings::AUTOSYNC_EVERY_CHAPTER) {
    if (lastSyncedSpine >= 0 && spineIndex <= lastSyncedSpine) return;
  } else {
    const uint8_t step = SETTINGS.getAutosyncPercentStep();
    if (step == 0) return;
    if (lastSyncedPercent >= 0.0f && bookPercent * 100.0f < lastSyncedPercent + static_cast<float>(step)) return;
  }
  armed = true;
}

bool isArmed() { return armed; }

void runIfArmed(GfxRenderer& renderer, uint32_t bookId, const std::string& epubPath, const std::string& epubCachePath,
                float bookPercent, int spineIndex, int pageNumber, int pageCount, int spineCount) {
  if (!armed) return;
  armed = false;  // Consumed regardless of outcome -- a failure retries on the next threshold crossing, not a spin.
  lastSyncedSpine = spineIndex;
  lastSyncedPercent = bookPercent * 100.0f;
  if (bookId == 0) return;

  performPush(renderer, bookId, epubPath, epubCachePath, bookPercent, spineIndex, pageNumber, pageCount, spineCount);
}

void runOnExit(GfxRenderer& renderer, uint32_t bookId, const std::string& epubPath, const std::string& epubCachePath,
              float bookPercent, int spineIndex, int pageNumber, int pageCount, int spineCount) {
  armed = false;
  if (bookId == 0 || SETTINGS.autosyncMode != CrossPointSettings::AUTOSYNC_ON_EXIT) return;

  performPush(renderer, bookId, epubPath, epubCachePath, bookPercent, spineIndex, pageNumber, pageCount, spineCount);
}

}  // namespace ProgressAutoSync
