#pragma once

#include "activities/UiListActivity.h"
#include <string>
#include <vector>

// Touch-first replacement for EpubReaderActivity's old renderAo3EndOfBook()/handleAo3EndOfBookInput()
// (button-only: Confirm checked for updates, Back opened the series view, nothing was reachable by
// tap). Launched once when an AO3 fic reaches its end-of-book screen; EpubReaderActivity stays
// paused underneath until this activity returns or navigates away.
//
// The Update row does NOT launch AO3SyncActivity itself: that activity's WiFi connect pulls in
// NetworkEvents/WifiSelectionActivity, and launching it from here would leave this activity (a full
// FreeInkUI app) resident on the stack underneath it -- one heap-resident activity deeper than the
// EpubReaderActivity -> AO3SyncActivity -> WifiSelectionActivity chain this device has actually been
// proven to handle. Confirmed by a real crash in eink-qemu (Guru Meditation / InstrFetchProhibited
// inside NetworkEvents' own event-callback init, reproducible every time) when this activity stayed
// alive through that chain. Instead, Update sets APP_STATE.ao3EndOfBookUpdateRequested and finishes;
// EpubReaderActivity's own launchAo3UpdateCheck() (restored to do exactly what it did before this
// screen existed) picks it up from there, one level shallower, after this activity's memory is freed.
class Ao3EndOfBookActivity final : public UiListActivity {
 public:
  Ao3EndOfBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                       std::string bookPath, std::string bookTitle, std::string bookAuthor,
                       bool isCompleted, std::string seriesName, int seriesPart, int ao3LibraryReturnIndex);

  void onEnter() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& uiScreen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;

  const char* headerTitle() const override { return nullptr; }  // nullptr = no header band
  void drawFooter() override;

 private:
  std::string bookPath;
  std::string bookTitle;
  std::string bookAuthor;
  bool isCompleted;
  std::string seriesName;
  int seriesPart;
  int ao3LibraryReturnIndex;
  unsigned long enterMs = 0;  // input is ignored briefly after entry (leftover page-turn input)

  enum ActionRow { ROW_GO_HOME = 0, ROW_AO3_PAGE, ROW_SERIES, ROW_UPDATE, ROW_ARCHIVE };
  std::vector<ActionRow> activeRows;

  void buildActiveRows();
  void launchSeriesActivity();
  void returnToLibraryOrBrowser();
};
