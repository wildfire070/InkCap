#pragma once
#include <string>

#include "activities/Activity.h"

// Shows a QR code for a fic's AO3 page (work ID from the EPUB's ao3-info sidecar)
// alongside its indexed title/author/tags. Confirm opens the fic in the reader.
class Ao3PageQrActivity final : public Activity {
 public:
  explicit Ao3PageQrActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
      : Activity("Ao3PageQr", renderer, mappedInput), filePath(std::move(filePath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string filePath;

  char title[128] = {};
  char author[128] = {};
  char tags[4][16] = {};
  uint16_t chapterCount = 0;
  char updatedDate[12] = {};

  std::string storyUrl;

  void loadMetadata();
};
