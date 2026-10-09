#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstdlib>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";
}  // namespace

int CalibreConnectActivity::batchLineCount() const {
  int count = static_cast<int>(batchSucceeded.size());
  if (!batchFailed.empty()) {
    count += 1 + static_cast<int>(batchFailed.size());  // +1 for the "M failed:" header line
  }
  return count;
}

void CalibreConnectActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  {
    // connectedIP/connectedSSID/currentUploadName/lastCompleteName/
    // lastProgressReceived/lastProgressTotal/lastCompleteAt are all read by
    // render() on the render task with no lock of its own on that side
    // either -- see the identical guard in loop()'s handleClient() path,
    // which documents this exact hazard for these same fields.
    RenderLock lock(*this);
    connectedIP.clear();
    connectedSSID.clear();
    lastProgressReceived = 0;
    lastProgressTotal = 0;
    currentUploadName.clear();
    lastCompleteName.clear();
    lastCompleteAt = 0;
    batchSucceeded.clear();
    batchFailed.clear();
    batchConfirmed = false;
    batchListTopIndex = 0;
  }
  lastHandleClientTime = 0;
  lastProcessedCompleteAt = 0;
  lastProcessedJobCompleteAt = 0;
  exitRequested = false;

#ifdef SIMULATOR
  if (std::getenv("CROSSINK_SIMULATOR_SMOKE_CALIBRE_BATCH") != nullptr) {
    // Test-only: skip real WiFi/server startup and render the batch summary
    // directly with synthetic data, so this screen (long scrolling list, bold
    // headers, failure entries) can be screenshot-verified from the simulator
    // without a physical device or a real Calibre transfer -- see
    // scripts/run_simulator_smoke_test.py --calibre-batch.
    RenderLock lock(*this);
    state = CalibreConnectState::SERVER_RUNNING;
    connectedSSID = "Simulator WiFi (fake)";
    connectedIP = "127.0.0.1";
    for (int i = 1; i <= 22; i++) {
      char name[64];
      snprintf(name, sizeof(name), "Test Fic Chapter %02d.epub", i);
      batchSucceeded.push_back(name);
    }
    batchFailed = {"Already On Device.epub: File already exists",
                   "Corrupted Upload.epub: Write failed - disk full?"};
    batchConfirmed = true;
    requestUpdate();
    return;
  }
#endif

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               // ActivityManager unlocks its RenderLock before invoking
                               // result handlers ("Handler may acquire its own lock") --
                               // same guard as above.
                               RenderLock lock(*this);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    {
      RenderLock lock(*this);
      connectedIP = WiFi.localIP().toString().c_str();
      connectedSSID = WiFi.SSID().c_str();
    }
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  library::invalidateLibraryIndex();
  Activity::onExit();

  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (returnToReader) {
      silentRestartToReader();
    } else {
      silentRestart();
    }
  }
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }

  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  {
    RenderLock lock;
    state = CalibreConnectState::SERVER_STARTING;
    requestUpdate();
  }

  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  RenderLock lock;
  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    requestUpdate();
  } else {
    state = CalibreConnectState::ERROR;
    requestUpdate();
  }
}

void CalibreConnectActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  // Scroll the batch summary's name list -- Up/Down move one line, a swipe moves
  // a full page. Harmless to process even while the list isn't on screen right
  // now (mid-upload-progress): render()'s own scrollListBy clamp on the next
  // paint keeps batchListTopIndex sane regardless.
  if (!batchSucceeded.empty() || !batchFailed.empty()) {
    int scrollDelta = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      scrollDelta = 1;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      scrollDelta = -1;
    } else if (mappedInput.hasTouch()) {
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        scrollDelta = batchListVisibleRows;
      } else if (swipe == MappedInputManager::SwipeDir::Down) {
        scrollDelta = -batchListVisibleRows;
      }
    }
    if (scrollDelta != 0) {
      RenderLock lock(*this);
      const int next = scrollListBy(batchListTopIndex, scrollDelta, batchListVisibleRows, batchLineCount());
      if (next != batchListTopIndex) {
        batchListTopIndex = next;
        requestUpdate();
      }
    }
  }

  if (webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("CAL", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    esp_task_wdt_reset();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) {
        esp_task_wdt_reset();
      }
      if ((i & 0x0F) == 0x0F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          exitRequested = true;
          break;
        }
      }
    }
    lastHandleClientTime = millis();

    const auto status = webServer->getWsUploadStatus();
    RenderLock lock;  // Publish upload strings and their counters together with respect to render().
    bool changed = false;
    {
      // currentUploadName/lastCompleteName/lastProgressReceived/lastProgressTotal/
      // lastCompleteAt are all read by render() on the render task with no lock of
      // its own on that side either -- this is the only place they're mutated, so
      // guard it here. Without this, render() reading currentUploadName mid-assignment
      // (a std::string reallocation, trivial for any real upload filename) is UB --
      // a torn pointer/length read or a dereference of already-freed heap memory.
      RenderLock lock(*this);
      if (status.inProgress) {
        if (status.received != lastProgressReceived || status.total != lastProgressTotal ||
            status.filename != currentUploadName) {
          lastProgressReceived = status.received;
          lastProgressTotal = status.total;
          currentUploadName = status.filename;
          changed = true;
        }
      } else if (lastProgressReceived != 0 || lastProgressTotal != 0) {
        lastProgressReceived = 0;
        lastProgressTotal = 0;
        currentUploadName.clear();
        changed = true;
      }
      // Only update lastCompleteAt if the server has a NEW value (not one we already processed)
      // This prevents restoring an old value after the 6s timeout clears it
      if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastProcessedCompleteAt) {
        lastCompleteAt = status.lastCompleteAt;
        lastCompleteName = status.lastCompleteName;
        lastProcessedCompleteAt = status.lastCompleteAt;  // Mark this value as processed
        changed = true;
      }
      if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
        lastCompleteAt = 0;
        lastCompleteName.clear();
        // Note: we DON'T reset lastProcessedCompleteAt here, so we won't re-process the old server value
        changed = true;
      }
      // The Calibre plugin reconnects the WebSocket per file with varying gaps
      // between files, so there's no reliable single signal for "the whole send
      // job is done" from the upload protocol alone (a fixed idle timeout was
      // tried and fired mid-job on real multi-book sends, showing a premature
      // partial count). A patched plugin can ping /api/calibre-job-done once its
      // own upload_books() call returns, which is authoritative -- but plenty of
      // people will be running the stock plugin, so this still has to degrade
      // gracefully to a live, continuously-updated tally when that never comes.

      // 1) New activity after a confirmed job means the next send has started --
      // clear the previous job's frozen display before the next result lands in
      // it. The server's live batch only has anything in it here because
      // handleCalibreJobDone() clears it on confirmation (step 3 below) and a
      // fresh upload_books() call is now adding to it again.
      if (batchConfirmed && (!status.batchSucceeded.empty() || !status.batchFailed.empty())) {
        batchSucceeded.clear();
        batchFailed.clear();
        batchConfirmed = false;
        batchListTopIndex = 0;
        changed = true;
      }

      // 2) Live running tally -- skipped once confirmed, so the server having
      // cleared its own live batch post-confirmation doesn't blank out the
      // summary just shown (the frozen snapshot stays as-is until step 1 above
      // detects genuinely new activity).
      if (!batchConfirmed && (status.batchSucceeded.size() != batchSucceeded.size() ||
                              status.batchFailed.size() != batchFailed.size())) {
        batchSucceeded = status.batchSucceeded;
        batchFailed = status.batchFailed;
        lastCompleteAt = 0;
        lastCompleteName.clear();
        changed = true;
      }

      // 3) Explicit, authoritative confirmation from a patched plugin. Locks the
      // display to this exact snapshot until step 1 reopens it for the next job.
      if (status.jobCompleteAt != 0 && status.jobCompleteAt != lastProcessedJobCompleteAt) {
        lastProcessedJobCompleteAt = status.jobCompleteAt;
        batchSucceeded = status.lastJobSucceeded;
        batchFailed = status.lastJobFailed;
        batchConfirmed = true;
        lastCompleteAt = 0;
        lastCompleteName.clear();
        changed = true;
      }
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    finish();
    return;
  }
}

void CalibreConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  CompactHeader::drawTitle(renderer, tr(STR_CALIBRE_WIRELESS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CALIBRE_STARTING));
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    const int subHeaderTop = CompactHeader::contentTop(renderer);
    GUI.drawSubHeader(renderer, Rect{0, subHeaderTop, pageWidth, metrics.tabBarHeight}, connectedSSID.c_str());

    // Keep the network name and full address independently readable on narrow
    // screens. Sharing one subheader row forces one of them to be truncated.
    const std::string ipLabel = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
    const int ipTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, ipTop, ipLabel.c_str());

    int y = ipTop + height + metrics.verticalSpacing * 3;
    const auto heightText12 = renderer.getTextHeight(UI_12_FONT_ID);
    const bool hasBatchResults = !batchSucceeded.empty() || !batchFailed.empty();

    // Once at least one file has landed, the connect instructions have already
    // served their purpose -- drop them to give the (potentially long) tally room.
    if (!hasBatchResults) {
      renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_SETUP), true,
                        EpdFontFamily::BOLD);
      y += heightText12 + metrics.verticalSpacing * 2;

      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_INSTRUCTION_1));
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height, tr(STR_CALIBRE_INSTRUCTION_2));
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height * 2, tr(STR_CALIBRE_INSTRUCTION_3));
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height * 3, tr(STR_CALIBRE_INSTRUCTION_4));

      y += height * 3 + metrics.verticalSpacing * 4;
    }

    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_STATUS), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    const bool showUploadProgress = lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal;
    if (showUploadProgress) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!currentUploadName.empty()) {
        label += ": " + currentUploadName;
        label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                       EpdFontFamily::REGULAR);
      }
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, label.c_str());
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, y + height + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          lastProgressReceived, lastProgressTotal);
      y += height + metrics.verticalSpacing * 2 + metrics.progressBarHeight;
    } else if (hasBatchResults) {
      char doneMsg[64];
      snprintf(doneMsg, sizeof(doneMsg), tr(STR_CALIBRE_DONE_FORMAT), static_cast<int>(batchSucceeded.size()));
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, doneMsg, true, EpdFontFamily::BOLD);
      y += height + metrics.verticalSpacing;

      // Flatten succeeded+failed into one scrollable list of lines (the "M
      // failed:" header, if any, is just another bold line in the same list) so
      // a long batch scrolls within this section instead of truncating with
      // "+N more" -- see batchListTopIndex/batchListVisibleRows in the header.
      struct SummaryLine {
        std::string text;
        bool bold;
      };
      std::vector<SummaryLine> lines;
      lines.reserve(batchLineCount());
      for (const auto& name : batchSucceeded) {
        lines.push_back({"- " + name, false});
      }
      if (!batchFailed.empty()) {
        char failedMsg[64];
        snprintf(failedMsg, sizeof(failedMsg), tr(STR_CALIBRE_FAILED_FORMAT), static_cast<int>(batchFailed.size()));
        lines.push_back({failedMsg, true});
        for (const auto& name : batchFailed) {
          lines.push_back({"- " + name, false});
        }
      }

      const int maxY = pageHeight - metrics.tabBarHeight;
      batchListVisibleRows = std::max(1, static_cast<int>((maxY - y) / height));
      const int totalLines = static_cast<int>(lines.size());
      batchListTopIndex = scrollListBy(batchListTopIndex, 0, batchListVisibleRows, totalLines);
      const bool needsScrollbar = totalLines > batchListVisibleRows;

      // Touch-capable devices (e.g. X4 Pro) never show GUI.drawButtonHints's
      // Up/Down labels below -- BaseTheme::drawButtonHints/drawSideButtonHints
      // both no-op when gpio.hasTouch() is true -- so that text hint alone
      // leaves no indication this list scrolls at all on those devices. Draw an
      // actual scrollbar, which works regardless of input method, reserving
      // room for it up front so line text doesn't run under it.
      constexpr int kScrollbarWidth = 5;
      constexpr int kScrollbarGap = 8;
      const int scrollbarReserve = needsScrollbar ? kScrollbarWidth + kScrollbarGap : 0;
      const int textMaxWidth = pageWidth - metrics.contentSidePadding * 2 - scrollbarReserve;
      const int listTop = y;

      const int lastVisible = std::min(totalLines, batchListTopIndex + batchListVisibleRows);
      for (int i = batchListTopIndex; i < lastVisible; i++) {
        std::string lineText = renderer.truncatedText(SMALL_FONT_ID, lines[i].text.c_str(), textMaxWidth,
                                                       EpdFontFamily::REGULAR);
        renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, lineText.c_str(), true,
                          lines[i].bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        y += height;
      }

      if (needsScrollbar) {
        const int trackTop = listTop;
        const int trackHeight = maxY - listTop;
        const int trackX = pageWidth - metrics.contentSidePadding - kScrollbarWidth;
        renderer.drawRect(trackX, trackTop, kScrollbarWidth, trackHeight);
        const int maxTopIndex = totalLines - batchListVisibleRows;
        const int thumbHeight = std::max(kScrollbarWidth * 2, trackHeight * batchListVisibleRows / totalLines);
        const int thumbTravel = trackHeight - thumbHeight;
        const int thumbY = trackTop + (maxTopIndex > 0 ? thumbTravel * batchListTopIndex / maxTopIndex : 0);
        renderer.fillRect(trackX, thumbY, kScrollbarWidth, thumbHeight);
      }
    } else if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                   EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, msg.c_str());
    }

    const bool canScrollBatchList = hasBatchResults && !showUploadProgress && batchLineCount() > batchListVisibleRows;
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "",
                                              canScrollBatchList ? tr(STR_DIR_UP) : "",
                                              canScrollBatchList ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}
