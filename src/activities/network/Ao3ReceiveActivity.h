#pragma once

#include <functional>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/CrossPointWebServer.h"

enum class Ao3ReceiveState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * Ao3ReceiveActivity starts the file transfer server in STA mode and shows
 * instructions for the Send to AvesO3 browser extension, which pushes AO3 EPUBs
 * straight to http://crosspoint.local/upload. Structurally the Calibre Wireless
 * activity with AO3-specific text.
 */
class Ao3ReceiveActivity final : public Activity {
  Ao3ReceiveState state = Ao3ReceiveState::WIFI_SELECTION;
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
  bool exitRequested = false;
  bool returnToReader = false;

  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();

 public:
  explicit Ao3ReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToReader = false)
      : Activity("Ao3Receive", renderer, mappedInput), returnToReader(returnToReader) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
