#pragma once

#include <functional>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/CrossPointWebServer.h"

enum class Ao3ReceiveState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, FILE_RECEIVED, ERROR };

// RECEIVE_NEW: the ordinary "Send to AvesO3 extension -> AO3 Receive folder -> review" flow.
// UPDATE_SINGLE: launched from the AO3 QR page's Update button for one already-indexed fic --
// the next upload is swapped into that fic's own path in place, no review step.
enum class Ao3ReceiveMode { RECEIVE_NEW, UPDATE_SINGLE };

/**
 * Ao3ReceiveActivity starts the file transfer server in STA mode and shows
 * instructions for the Send to AvesO3 browser extension, which pushes AO3 EPUBs
 * straight to http://crosspoint.local/upload. Structurally the Calibre Wireless
 * activity with AO3-specific text.
 */
class Ao3ReceiveActivity final : public Activity {
  Ao3ReceiveState state = Ao3ReceiveState::WIFI_SELECTION;
  Ao3ReceiveMode mode = Ao3ReceiveMode::RECEIVE_NEW;
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

  // UPDATE_SINGLE only
  std::string targetPath;
  std::string updateStoryUrl;
  std::string updateTitle;
  std::string updateAuthor;
  std::string errorMessage;

  void renderServerRunning() const;
  void renderUpdateRunning() const;
  void renderFileReceived() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();
  void handleUpdateComplete(const std::string& receivedPath);

 public:
  explicit Ao3ReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToReader = false)
      : Activity("Ao3Receive", renderer, mappedInput), returnToReader(returnToReader) {}

  Ao3ReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Ao3ReceiveMode mode,
                     std::string targetPath, std::string storyUrl, std::string title, std::string author)
      : Activity("Ao3Receive", renderer, mappedInput),
        mode(mode),
        targetPath(std::move(targetPath)),
        updateStoryUrl(std::move(storyUrl)),
        updateTitle(std::move(title)),
        updateAuthor(std::move(author)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
