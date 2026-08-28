#include "AO3SyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <ZipFile.h>

#include "HalStorage.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
// The update check runs a TLS session + streamed HTTP download alongside the 48 KB
// framebuffer and the paused reader — the tightest heap moment in the AO3 feature on
// the ESP32-C3 (no PSRAM). Gate on both total free heap and the largest allocatable
// block (TLS needs a contiguous record buffer) so a low/fragmented heap fails with a
// clear message instead of an OOM abort() mid-handshake. Mirrors Ao3IndexActivity.
constexpr uint32_t AO3_SYNC_MIN_FREE_HEAP = 90 * 1024;
constexpr uint32_t AO3_SYNC_MIN_MAX_ALLOC = 32 * 1024;
}  // namespace

void AO3SyncActivity::onEnter() {
  Activity::onEnter();

  if (ESP.getFreeHeap() < AO3_SYNC_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < AO3_SYNC_MIN_MAX_ALLOC) {
    // A loaded SD custom font can be the difference here; release it and
    // recheck before giving up. (Not releasing the framebuffer too: render()
    // runs on a separate task, and nothing guarantees whatever render got
    // queued getting into this activity has actually finished — releasing
    // while it's still in flight is a confirmed crash, not a hypothetical
    // one; see BookFusionBrowserActivity::downloadBook()'s fix. The
    // streaming-read release further down is safe because it's preceded by
    // a genuinely-waited requestUpdateAndWait(), not just requestUpdate().)
    sdFontSystem.releaseForNetwork(renderer);
  }
  if (ESP.getFreeHeap() < AO3_SYNC_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < AO3_SYNC_MIN_MAX_ALLOC) {
    LOG_ERR("AO3", "Insufficient heap for update check: free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    errorMessage = "Not enough memory";
    state = AO3SyncState::ERROR;
    requestUpdate();
    return;
  }

  WiFi.mode(WIFI_STA);

  state = AO3SyncState::CONNECTING_WIFI;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& res) { onWifiSelectionComplete(!res.isCancelled); });
}

void AO3SyncActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
}

void AO3SyncActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    errorMessage = "WiFi Failed";
    state = AO3SyncState::ERROR;
    requestUpdate();
    return;
  }

  if (state == AO3SyncState::DOWNLOADING) {
    // We were trying to retry a download
    requestUpdateAndWait();
    performDownload();
  } else {
    state = AO3SyncState::SEARCHING;
    requestUpdateAndWait();
    performSearch();
  }
}

void AO3SyncActivity::performSearch() {
  if (workId.empty()) {
    errorMessage = "Invalid Work ID";
    state = AO3SyncState::ERROR;
    return;
  }

  std::string cleanWorkId = workId;
  cleanWorkId.erase(0, cleanWorkId.find_first_not_of(" \n\r\t"));
  cleanWorkId.erase(cleanWorkId.find_last_not_of(" \n\r\t") + 1);

  if (cleanWorkId.empty()) {
    errorMessage = "Invalid Work ID";
    state = AO3SyncState::ERROR;
    return;
  }

  // Release right before the real request: the Wi-Fi selection screen and the
  // "Searching" status screen render on the way here and can lazily reload the
  // SD font, so releasing any earlier doesn't reliably free memory for TLS.
  sdFontSystem.releaseForNetwork(renderer);

  // .org is the official domain and the one most likely to resolve/connect
  // reliably; .gay is an unofficial mirror, kept only as a fallback for when
  // .org is unreachable or blocks the request.
  usingGayFallback = false;
  const std::string searchUrls[] = {"https://archiveofourown.org/works/" + cleanWorkId + "?view_adult=true",
                                    "https://archiveofourown.gay/works/" + cleanWorkId + "?view_adult=true"};

  // AO3 is Cloudflare-fronted. The mbedTLS-based clients this used to use
  // (Arduino's HTTPClient/NetworkClientSecure, then ESP-IDF's esp_http_client
  // for downloads) got their connections silently dropped by Cloudflare's TLS
  // fingerprinting — no clean rejection, just a ~2-minute hang before a
  // transport error, confirmed on real hardware on a network that reaches AO3
  // fine from a browser. wolfSSL is the stack BookFusion already gets through
  // comparable protection with on the same device/network, so every AO3
  // network call now goes through the same freeink::SecureHttpClient.
  const auto shouldAbort = [this] {
    mappedInput.update();
    return mappedInput.wasReleased(MappedInputManager::Button::Back);
  };

  int status_code = 0;
  bool userAborted = false;
  for (int urlIdx = 0; urlIdx < 2; urlIdx++) {
    if (urlIdx == 1) {
      usingGayFallback = true;
      requestUpdateAndWait();
      delay(1000);
    }
    const std::string& currentUrl = searchUrls[urlIdx];
    int max_retries = 3;

    std::string htmlAcc;
    bool foundDate = false;
    bool foundChapters = false;

    while (max_retries > 0) {
      freeink::SecureHttpClient http;
      http.setInsecure();  // Skip strict cert validation, matching the old client's behavior
      http.setTimeout(20000);
      http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/120.0.0.0 Safari/537.36");
      http.setFollowRedirects(5);

      htmlAcc.clear();
      foundDate = false;
      foundChapters = false;
      bytesProcessed = 0;

      if (!http.begin(currentUrl)) {
        status_code = -1;
      } else {
        http.addHeader("Accept",
                       "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8");
        http.addHeader("Accept-Language", "en-US,en;q=0.5");
        // AO3's redirect from /works/<id> to /works/<id>/chapters/<id> drops
        // the ?view_adult=true query string, and SecureHttpClient has no
        // cookie jar to carry the Set-Cookie it issues instead - without this,
        // the redirected request lands on the age-confirmation interstitial
        // rather than the real chapter page. Headers persist across redirect
        // hops in this client, so sending the cookie explicitly survives it.
        http.addHeader("Cookie", "view_adult=true");

        // Release the e-ink framebuffer(s) for the whole request: the TLS
        // handshake is the tightest heap moment (see the framebuffer-release
        // work elsewhere in this file/BookFusionBrowserActivity), and nothing
        // in shouldAbort/onData below renders, so it's safe to hold released
        // for the connect, headers, and streamed body alike. Reallocates
        // automatically when this scope ends, on every exit path.
        GfxRenderer::NetworkBufferLoan fbLoan(renderer);
        const auto onData = [this, &htmlAcc, &foundDate, &foundChapters](const uint8_t* data, size_t len) {
          bytesProcessed += len;
          htmlAcc.append(reinterpret_cast<const char*>(data), len);

          // Search for date
          if (!foundDate) {
            size_t pos = htmlAcc.find("<dd class=\"status\">");
            if (pos != std::string::npos) {
              size_t endPos = htmlAcc.find("</dd>", pos);
              if (endPos != std::string::npos) {
                scrapedDate = htmlAcc.substr(pos + 19, endPos - (pos + 19));
                foundDate = true;
              }
            }
          }

          // Search for chapters
          if (!foundChapters) {
            size_t pos = htmlAcc.find("<dd class=\"chapters\">");
            if (pos != std::string::npos) {
              size_t endPos = htmlAcc.find("</dd>", pos);
              if (endPos != std::string::npos) {
                std::string chapStr = htmlAcc.substr(pos + 21, endPos - (pos + 21));
                size_t slashPos = chapStr.find("/");
                if (slashPos != std::string::npos) {
                  std::string current = chapStr.substr(0, slashPos);
                  std::string total = chapStr.substr(slashPos + 1);
                  scrapedIsCompleted = total != "?" && current == total;
                  foundChapters = true;
                }
              }
            }
          }

          // Bound memory growth now that both markers have been searched for
          // in the full buffer. Keep a tail long enough to catch a marker
          // that straddles this chunk boundary and the next one.
          if (htmlAcc.size() > 4096) {
            htmlAcc = htmlAcc.substr(htmlAcc.size() - 256);
          }

          // Stop once we have both markers, or as a safety cap matching the
          // old loop's 100 KB ceiling.
          return !(foundDate && foundChapters) && bytesProcessed < 100000;
        };

        status_code = http.GET(onData, shouldAbort);
        LOG_INF("AO3", "GET %s -> status=%d bytes=%u aborted=%d", currentUrl.c_str(), status_code,
                (unsigned)bytesProcessed, http.aborted());
        if (http.aborted()) {
          // shouldAbort() already consumed the Back-release event, so the
          // between-retries check below would never see it — capture the
          // abort here instead of letting it look like a generic failure.
          userAborted = true;
        }
      }

      if (userAborted) break;

      if (status_code == 200 || status_code == 403 || status_code == 404) {
        break;  // Success or definite non-retryable error
      }

      LOG_INF("AO3", "HTTP error %d, retries left: %d", status_code, max_retries - 1);
      max_retries--;

      if (max_retries > 0) {
        // Check if user wants to cancel while retrying
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          userAborted = true;
          break;
        }
        delay(1500);  // Wait before retry
      }
    }

    if (userAborted) {
      errorMessage = "Search Aborted";
      state = AO3SyncState::ERROR;
      requestUpdate();
      return;
    }

    if (status_code == 403) {
      if (urlIdx == 0) continue;  // try .gay
      errorMessage = tr(STR_AO3_ERROR_LOCKED);
      state = AO3SyncState::ERROR;
      return;
    } else if (status_code == 429) {
      errorMessage = "AO3 Rate Limit: Try later";
      state = AO3SyncState::ERROR;
      return;
    } else if (status_code == 404) {
      errorMessage = "Work Deleted/Not Found";
      state = AO3SyncState::ERROR;
      return;
    } else if (status_code != 200) {
      errorMessage = "Err: " + std::to_string(status_code);
      state = AO3SyncState::ERROR;
      return;
    }

    if (foundDate && foundChapters) {
      if (scrapedDate > currentLocalDate) {
        state = AO3SyncState::UPDATE_FOUND;
      } else {
        state = AO3SyncState::UP_TO_DATE;
      }
    } else {
      LOG_INF("AO3", "Parse failed: status=%d bytes=%u foundDate=%d foundChapters=%d",
              status_code, (unsigned)bytesProcessed, foundDate, foundChapters);
      errorMessage = tr(STR_AO3_ERROR_GENERIC);
      state = AO3SyncState::ERROR;
    }
    requestUpdate();
    break;
  }
}

void AO3SyncActivity::performDownload() {
  state = AO3SyncState::DOWNLOADING;
  errorMessage = "";
  downloadProgress = 0;
  downloadTotal = 0;
  // Must actually wait (not just requestUpdate()): the framebuffer release
  // just below frees the buffer this render may still be reading from
  // mid-flight otherwise — the same null-framebuffer store fault found and
  // fixed in BookFusionBrowserActivity::downloadBook().
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("AO3", "Downloading screen could not be rendered before fetch");
    requestUpdate(true);
  }

  // Same reasoning as performSearch(): release right before the real request.
  sdFontSystem.releaseForNetwork(renderer);

  // Same reasoning as performSearch(): .org first, .gay only as a fallback.
  std::string downloadUrl = "https://archiveofourown.org/downloads/" + workId + "/work.epub?v=" + scrapedDate;
  std::string tempPath = bookPath + ".tmp";

  LOG_INF("AO3", "Downloading: %s -> %s", downloadUrl.c_str(), tempPath.c_str());

  // Same framebuffer reasoning as BookFusionBrowserActivity::downloadBook():
  // free it for the whole transfer, reacquiring only to draw each throttled
  // progress update.
  const auto progressCallback = [this](size_t downloaded, size_t total) {
    downloadProgress = downloaded;
    downloadTotal = total;
    if (!renderer.reallocFrameBuffersAfterNetwork()) {
      LOG_ERR("AO3", "Framebuffer realloc failed during download progress");
      ESP.restart();
    }
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("AO3", "Download progress screen could not be rendered");
      requestUpdate(true);
    }
    renderer.releaseFrameBuffersForNetwork();
  };

  // Same reasoning as performSearch(): AO3 is Cloudflare-fronted, and
  // Cloudflare's TLS fingerprinting silently drops connections from the
  // default ESP_HTTP (mbedTLS) transport. WOLFSSL is the transport
  // BookFusion already gets through comparable protection with.
  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;

  renderer.releaseFrameBuffersForNetwork();
  auto result = HttpDownloader::downloadToFile(downloadUrl, tempPath, progressCallback, nullptr, "", "", downloadOptions);

  if (result == HttpDownloader::HTTP_ERROR) {
    usingGayFallback = true;
    if (!renderer.reallocFrameBuffersAfterNetwork()) {
      LOG_ERR("AO3", "Framebuffer realloc failed before gay fallback");
      ESP.restart();
    }
    requestUpdateAndWait();
    delay(1000);
    downloadUrl = "https://archiveofourown.gay/downloads/" + workId + "/work.epub?v=" + scrapedDate;
    renderer.releaseFrameBuffersForNetwork();
    result = HttpDownloader::downloadToFile(downloadUrl, tempPath, progressCallback, nullptr, "", "", downloadOptions);
  }

  // The buffer is left released by progressCallback regardless of how the
  // transfer ended; bring it back before any of the result-handling below
  // renders.
  if (!renderer.reallocFrameBuffersAfterNetwork()) {
    LOG_ERR("AO3", "Framebuffer realloc failed after download");
    ESP.restart();
  }

  if (result == HttpDownloader::OK) {
    LOG_INF("AO3", "Download successful, verifying ZIP integrity");

    ZipFile zip(tempPath);
    if (!zip.open()) {
      LOG_ERR("AO3", "ZIP Integrity check failed - truncated download?");
      if (Storage.exists(tempPath.c_str())) {
        Storage.remove(tempPath.c_str());
      }
      errorMessage = "Integrity Check Failed";
      state = AO3SyncState::ERROR;
      requestUpdate();
      return;
    }
    zip.close();

    LOG_INF("AO3", "Integrity verified, performing atomic swap");

    // Atomic Swap
    if (Storage.exists(bookPath.c_str())) {
      Storage.remove(bookPath.c_str());
    }

    if (Storage.rename(tempPath.c_str(), bookPath.c_str())) {
      LOG_INF("AO3", "Atomic swap complete");

      // Success result
      AO3Result res;
      res.scrapedDate = scrapedDate;
      res.isCompleted = scrapedIsCompleted;
      res.updateFound = true;
      res.downloaded = true;
      setResult(ActivityResult(res));
      finish();
    } else {
      errorMessage = "File Swap Failed";
      state = AO3SyncState::ERROR;
    }
  } else {
    LOG_ERR("AO3", "Download failed with error code: %d", result);
    if (Storage.exists(tempPath.c_str())) {
      Storage.remove(tempPath.c_str());
    }
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    state = AO3SyncState::ERROR;
  }
  requestUpdate();
}

void AO3SyncActivity::loop() {
  if (state == AO3SyncState::UPDATE_FOUND || state == AO3SyncState::UP_TO_DATE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (state == AO3SyncState::UPDATE_FOUND) {
        performDownload();
      } else {
        AO3Result res;
        res.scrapedDate = scrapedDate;
        res.isCompleted = scrapedIsCompleted;
        res.updateFound = false;
        setResult(ActivityResult(res));
        finish();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (state == AO3SyncState::UPDATE_FOUND) {
        // Signal the update exists so status becomes NEW_CHAPTER_AVAILABLE
        AO3Result res;
        res.updateFound = true;
        setResult(ActivityResult(res));
      }
      finish();
    }
  } else if (state == AO3SyncState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Check if WiFi is still connected, if not reconnect, otherwise just retry
      if (WiFi.status() == WL_CONNECTED) {
        if (downloadTotal > 0 || !scrapedDate.empty()) {
          // We already found the update but failed download
          performDownload();
        } else {
          state = AO3SyncState::SEARCHING;
          requestUpdateAndWait();
          performSearch();
        }
      } else {
        state = AO3SyncState::CONNECTING_WIFI;
        requestUpdate();
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& res) { onWifiSelectionComplete(!res.isCancelled); });
      }
    }
  }
}

void AO3SyncActivity::renderInitializing() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto top = (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AO3_CONNECTING_WIFI));
}

void AO3SyncActivity::renderSearching() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto top = (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  if (usingGayFallback) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Retrying on .gay domain...");
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AO3_SEARCHING));
  }
}

void AO3SyncActivity::renderDownloading() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = pageHeight / 2 - 40;

  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_DOWNLOADING), true, EpdFontFamily::BOLD);

  if (downloadTotal > 0) {
    const int barWidth = pageWidth - 100;
    constexpr int barHeight = 20;
    const int barX = 50;
    const int barY = pageHeight / 2;
    GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);
  }
}

void AO3SyncActivity::renderResult() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AO3SyncState::UP_TO_DATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AO3_UP_TO_DATE), true, EpdFontFamily::BOLD);
    GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_DONE), "", "");
  } else if (state == AO3SyncState::UPDATE_FOUND) {
    renderer.drawCenteredText(UI_10_FONT_ID, top - 10, tr(STR_AO3_UPDATE_QUERY), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 5, (std::string("New date: ") + scrapedDate).c_str());
    GUI.drawButtonHints(renderer, tr(STR_CANCEL), tr(STR_AO3_DOWNLOAD), "", "");
  }
}

void AO3SyncActivity::renderError() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto top = (pageHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, top, errorMessage.c_str(), true, EpdFontFamily::BOLD);
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_RETRY), "", "");
}

void AO3SyncActivity::render(RenderLock&& lock) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AO3_SEARCH));

  switch (state) {
    case AO3SyncState::INITIALIZING:
    case AO3SyncState::CONNECTING_WIFI:
      renderInitializing();
      break;
    case AO3SyncState::SEARCHING:
      renderSearching();
      break;
    case AO3SyncState::DOWNLOADING:
      renderDownloading();
      break;
    case AO3SyncState::UP_TO_DATE:
    case AO3SyncState::UPDATE_FOUND:
      renderResult();
      break;
    case AO3SyncState::ERROR:
      renderError();
      break;
  }

  renderer.displayBuffer();
}
