#pragma once

class GfxRenderer;
class HalGPIO;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power };
  enum class SwipeDir { None, Left, Right, Up, Down };
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO&, const GfxRenderer& renderer) : renderer(renderer) {}

  const GfxRenderer& getRenderer() const { return renderer; }

  bool wasScreenTouchDown(int& x, int& y) const {
    if (!touchDown) return false;
    x = touchX;
    y = touchY;
    touchDown = false;
    return true;
  }

  bool wasScreenTapped(int& x, int& y) const {
    if (!touchRelease) return false;
    touchRelease = false;
    if (suppressTouchTap) {
      suppressTouchTap = false;
      return false;
    }
    x = touchX;
    y = touchY;
    return true;
  }

  SwipeDir wasSwipe() const { return SwipeDir::None; }
  bool wasPressed(const Button button) const {
    if (button != Button::Confirm || !confirmPressed) return false;
    confirmPressed = false;
    return true;
  }
  bool wasReleased(Button) const { return false; }
  bool isPressed(const Button button) const { return button == Button::Power && powerPressed; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const {
    return {back, confirm, previous, next};
  }

  void suppressNextTouchTap() { suppressTouchTap = true; }
  void suppressNextConfirmRelease() {}
  void suppressNextBackRelease() {}
  void suppressNextPowerRelease() { powerReleaseSuppressed = true; }

  void injectTouchDown(const int x, const int y) {
    touchX = x;
    touchY = y;
    touchDown = true;
  }

  void injectTouchRelease(const int x, const int y) {
    touchX = x;
    touchY = y;
    touchRelease = true;
  }

  void injectPowerConfirmPress() {
    confirmPressed = true;
    powerPressed = true;
  }

  bool isPowerReleaseSuppressed() const { return powerReleaseSuppressed; }

 private:
  const GfxRenderer& renderer;
  mutable bool touchDown = false;
  mutable bool touchRelease = false;
  mutable bool suppressTouchTap = false;
  mutable bool confirmPressed = false;
  mutable bool powerPressed = false;
  mutable bool powerReleaseSuppressed = false;
  mutable int touchX = 0;
  mutable int touchY = 0;
};
