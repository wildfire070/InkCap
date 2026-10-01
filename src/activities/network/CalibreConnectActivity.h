#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/CrossPointWebServer.h"

enum class CalibreConnectState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * CalibreConnectActivity starts the file transfer server in STA mode,
 * but renders Calibre-specific instructions instead of the web transfer UI.
 */
class CalibreConnectActivity final : public Activity {
  CalibreConnectState state = CalibreConnectState::WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;

  std::unique_ptr<CrossPointWebServer> webServer;
  std::string connectedIP;
  std::string connectedSSID;
  unsigned long lastHandleClientTime = 0;
  size_t lastProgressReceived = 0;
  size_t lastProgressTotal = 0;
  std::string currentUploadName;
  std::string lastCompleteName;
  unsigned long lastCompleteAt = 0;
  unsigned long lastProcessedCompleteAt = 0;  // Track which server value we've already processed
  unsigned long lastProcessedJobCompleteAt = 0;  // Track which job-done ping we've already processed
  // Running tally for this screen visit, mirrored from the server and shown
  // instead of the single-file toast once non-empty. While batchConfirmed is
  // false this just tracks the server's live, still-growing batch (the only
  // option for an unpatched Calibre plugin); once a job-done ping arrives (see
  // loop()), it's locked to that frozen, confirmed snapshot until new activity
  // after it signals the next job has started.
  std::vector<std::string> batchSucceeded;
  std::vector<std::string> batchFailed;
  bool batchConfirmed = false;
  // Scroll state for the batch summary's name list -- topIndex into the flattened
  // succeeded+failed line list, visibleRows computed during the last render() and
  // reused by loop() to clamp input-driven scrolling (see scrollListBy in
  // components/UiAppHelpers.h, the convention every other list screen uses).
  int batchListTopIndex = 0;
  int batchListVisibleRows = 1;
  bool exitRequested = false;
  bool returnToReader = false;

  int batchLineCount() const;
  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();

 public:
  explicit CalibreConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToReader = false)
      : Activity("CalibreConnect", renderer, mappedInput), returnToReader(returnToReader) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
