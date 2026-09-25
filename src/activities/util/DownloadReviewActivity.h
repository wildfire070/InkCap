#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/DownloadReview.h"

/**
 * Reviews books downloaded from BookFusion: for each one, records its IDs (loading its metadata),
 * looks for another copy of the same book already on the device, and if there is one asks whether to
 * replace it. "Keep both" leaves the new file under its " (2)" name.
 *
 * Runs after the post-download restart, from the Home screen: loading a book needs heap that a live
 * Wi-Fi session doesn't leave.
 */
class DownloadReviewActivity final : public Activity {
  enum class State { WORKING, ASKING, MEMORY_ERROR };

  State state = State::WORKING;
  std::vector<DownloadReview::Entry> queue;
  size_t total = 0;
  size_t processed = 0;
  std::string currentName;

  void processFront();
  void dropFront();
  void askAboutDuplicate(const std::string& newPath, const std::string& oldPath, const std::string& title,
                         uint32_t bookFusionId);

 public:
  explicit DownloadReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DownloadReview", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
