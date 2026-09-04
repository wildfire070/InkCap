#pragma once

#include <array>
#include <functional>

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Button = MappedInputManager::Button;
  using Buttons = std::array<Button, 2>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously() const;

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);
  // Single-button overload: NOT the same as calling the Buttons& one with a
  // one-element brace-init like {button} -- that aggregate-initializes the
  // array's second slot to Button{}, which is Button::Back (enum value 0,
  // the first enumerator), so every one-button call written that way was
  // silently also matching a Back press/hold/release. This overload checks
  // only the one button given, no matter how it's called.
  void onPressAndContinuous(Button button, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);
  void onPress(Button button, const Callback& callback);  // see onPressAndContinuous(Button, ...)'s comment

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);
  void onRelease(Button button, const Callback& callback);  // see onPressAndContinuous(Button, ...)'s comment

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);
  void onContinuous(Button button, const Callback& callback);  // see onPressAndContinuous(Button, ...)'s comment

  // AO3 library: front/side-specific continuous-hold navigation, independent of
  // the generic Next/Previous mapping above.
  void onFrontNextContinuous(const Callback& callback);
  void onFrontPreviousContinuous(const Callback& callback);
  void onSideNextContinuous(const Callback& callback);
  void onSidePreviousContinuous(const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  [[nodiscard]] static constexpr Buttons getNextButtons() {
    return {MappedInputManager::Button::Down, MappedInputManager::Button::Right};
  }
  [[nodiscard]] static constexpr Buttons getPreviousButtons() {
    return {MappedInputManager::Button::Up, MappedInputManager::Button::Left};
  }
};
