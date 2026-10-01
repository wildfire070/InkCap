#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";
}  // namespace

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
  }
  lastHandleClientTime = 0;
  lastProcessedCompleteAt = 0;
  exitRequested = false;

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
  state = CalibreConnectState::SERVER_STARTING;
  requestUpdate();

  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

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
      // The Calibre plugin reconnects the WebSocket per file with varying gaps between
      // files, so there's no reliable single signal for "the whole send job is done"
      // (a fixed idle timeout was tried and fired mid-job on real multi-book sends,
      // showing a premature partial count). Instead, just mirror the server's running
      // tally live for as long as this screen stays open, per the on-screen instruction
      // to keep it open while sending -- whatever's showing when the user is done
      // watching is the real, complete total, because nothing ever resets it early.
      if (status.batchSucceeded.size() != batchSucceeded.size() ||
          status.batchFailed.size() != batchFailed.size()) {
        batchSucceeded = status.batchSucceeded;
        batchFailed = status.batchFailed;
        // The running tally has taken over from the single-file toast.
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
    const int subHeaderTop = CompactHeader::contentTop(metrics);
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
      // Bounds-checked list: stop (and say "+N more") rather than draw into the
      // button hint row at the bottom of the screen.
      const int maxY = pageHeight - metrics.tabBarHeight;
      auto drawNameList = [&](const std::vector<std::string>& names) {
        size_t shown = 0;
        for (; shown < names.size() && y + height <= maxY; shown++) {
          std::string line = "- " + names[shown];
          line = renderer.truncatedText(SMALL_FONT_ID, line.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                        EpdFontFamily::REGULAR);
          renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, line.c_str());
          y += height;
        }
        if (shown < names.size() && y + height <= maxY) {
          char moreMsg[32];
          snprintf(moreMsg, sizeof(moreMsg), tr(STR_CALIBRE_AND_MORE_FORMAT),
                   static_cast<int>(names.size() - shown));
          renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, moreMsg);
          y += height;
        }
      };

      char doneMsg[64];
      snprintf(doneMsg, sizeof(doneMsg), tr(STR_CALIBRE_DONE_FORMAT), static_cast<int>(batchSucceeded.size()));
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, doneMsg, true, EpdFontFamily::BOLD);
      y += height + metrics.verticalSpacing;
      drawNameList(batchSucceeded);

      if (!batchFailed.empty()) {
        y += metrics.verticalSpacing;
        char failedMsg[64];
        snprintf(failedMsg, sizeof(failedMsg), tr(STR_CALIBRE_FAILED_FORMAT), static_cast<int>(batchFailed.size()));
        renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, failedMsg, true, EpdFontFamily::BOLD);
        y += height + metrics.verticalSpacing;
        drawNameList(batchFailed);
      }
    } else if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                   EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, msg.c_str());
    }

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}
