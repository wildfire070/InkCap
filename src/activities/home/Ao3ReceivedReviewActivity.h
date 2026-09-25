#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

/**
 * Reviews fics received through AO3 Receive. For each pending file it indexes the fic
 * (which reads its AO3 work ID, even for AO3's own downloads whose metadata lacks one),
 * looks for another copy of the same work already in the AO3 library, and if there is
 * one asks whether to replace it. "Keep both" leaves the new file in the receive folder.
 *
 * Runs after the post-transfer restart rather than during the transfer: indexing needs
 * heap that a live Wi-Fi session doesn't leave.
 */
class Ao3ReceivedReviewActivity final : public Activity {
  enum class State { WORKING, ASKING, MEMORY_ERROR };

  State state = State::WORKING;
  std::vector<std::string> queue;
  size_t total = 0;
  size_t processed = 0;
  std::string currentName;

  void processFront();
  void dropFront();
  void askAboutDuplicate(const std::string& newPath, const std::string& oldPath, const std::string& title);
  static bool indexFic(const std::string& path);
  static void nameFromMetadata(const std::string& path);

 public:
  explicit Ao3ReceivedReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Ao3ReceivedReview", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
