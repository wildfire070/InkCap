#pragma once
#include <string>
#include <vector>

#include "../../util/ButtonNavigator.h"
#include "../Activity.h"

// Generic SD-card folder browser/picker, single- or multi-select. Used for
// the AO3 library folder, AO3 archive folder, AO3 library exclusions, the
// BookFusion download folder, and Cache Exclusions -- despite the name's
// history, this logic was never AO3-specific.
enum class PickerMode { SINGLE, MULTI };

class FolderPickerActivity final : public Activity {
  std::string title;
  PickerMode mode;
  std::string currentPath;
  std::vector<std::string> directories;
  std::vector<std::string> selectedPaths;

  size_t selectorIndex = 0;
  ButtonNavigator buttonNavigator;

  void loadDirectories();
  bool isSelected(const std::string& path) const;
  void toggleSelection(const std::string& path);

 public:
  FolderPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, PickerMode mode,
                       std::vector<std::string> initialSelected = {}, std::string startPath = "/")
      : Activity("FolderPicker", renderer, mappedInput),
        title(std::move(title)),
        mode(mode),
        currentPath(std::move(startPath)),
        selectedPaths(std::move(initialSelected)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
