#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "activities/Activity.h"
#include "activities/reader/EpubReaderMenuModel.h"
#include "util/ButtonNavigator.h"

/**
 * Settings -> Reader -> Customise Reader Menu: lets the user hide any row the reader
 * drawer's More/Location/Settings tabs can show (Font/Layout tab rows are settings
 * pickers, not optional actions, and are not listed here). Persists as a single bitmask,
 * CrossPointSettings::readerMenuHiddenRows -- see readerDrawerItemVisibilityBit() in
 * EpubReaderMenuModel.h for the bit assignment. Visibility here is only ever a veto: a
 * hidden row that the book/device state wouldn't show anyway stays absent regardless.
 * Lists CycleStatus alongside ToggleCompleted/ToggleArchived even though only one of
 * those ever appears for a given book (isAo3Book) -- the hide decision isn't book-specific.
 */
class ReaderMenuVisibilityActivity final : public Activity {
 public:
  explicit ReaderMenuVisibilityActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<16, 4>;

  // Every item readerDrawerItemVisibilityBit() assigns a bit to, in catalog order --
  // built once in onEnter() so row index and ReaderDrawerCatalogItem stay in lockstep
  // without re-deriving the list on every render.
  std::array<ReaderDrawerCatalogItem, 32> rows{};
  size_t rowCount = 0;

  ButtonNavigator buttonNavigator;
  freeink::ui::ListNav listNav;

  size_t selectedIndex = 0;
  int topIndex = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  bool ignoreInitialConfirmRelease = false;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void provideRow(void* user, uint16_t row, freeink::ui::ListItem& item);
  void fillRow(uint16_t row, freeink::ui::ListItem& item);
  void buildListScreen(UiApp::ScreenType& screen);
  void moveSelection(size_t next);
  void toggleRow(size_t row);
};
