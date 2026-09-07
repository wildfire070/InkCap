#pragma once

#include <string>
#include <vector>

#include "BookStatus.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BookActionActivity final : public Activity {
  std::string filePath;
  std::string fileName;
  int selectorIndex = 0;
  static constexpr int ROW_COUNT = 5;
  BookStatus currentStatus = BookStatus::START;
  BookStatus initialStatus = BookStatus::START;
  ButtonNavigator buttonNavigator;
  bool hasAo3LibraryInfo = false;
  bool bookIsArchived = false;
  // Set when Mark/Unmark for Later toggles -- currentStatus alone doesn't
  // change from this, so the Back handler needs a separate signal to know a
  // result should still be reported (see BookActionResult::markedForLaterChanged).
  bool markedForLaterChanged = false;

 public:
  BookActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                     std::string fileName);

  void onEnter() override;
  void render(RenderLock&& lock) override;
  void loop() override;

 private:
  void saveStatus();
};
