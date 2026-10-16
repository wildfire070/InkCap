#if defined(SIMULATOR) && CROSSINK_APP_CAP_TOUCH
#include "ClippingSelectionSmokeTest.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "ClippingStore.h"
#include "activities/ActivityManager.h"
#include "activities/reader/DictionaryWordSelectActivity.h"
#include "activities/reader/EpubReaderActivity.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
void require(bool condition, const char* message) {
  if (condition) return;
  LOG_ERR("SMOKE", "Clipping selection: %s", message);
  std::_Exit(1);
}

void capture(const char* name) {
  const char* directory = std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES");
  if (!directory) return;
  std::filesystem::create_directories(directory);
  const auto path = std::filesystem::path(directory) / (std::string(name) + ".pgm");
  FILE* output = std::fopen(path.c_str(), "wb");
  require(output, "Cannot create screenshot");
  RenderLock lock;
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  std::fprintf(output, "P5\n%d %d\n255\n", width, height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, output);
  std::fclose(output);
}
void verifyOverlayBounds() {
  RenderLock lock;
  const auto orientation = renderer.getOrientation();
  const auto scale = SETTINGS.uiScale;
  for (int rotation = 0; rotation < 4; ++rotation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
    for (int size = 0; size < CrossPointSettings::UI_SCALE_COUNT; ++size) {
      SETTINGS.uiScale = static_cast<CrossPointSettings::UI_SCALE>(size);
      const auto safe = TextSelectionOverlay::safeArea(renderer);
      for (const auto point : {TextSelectionOverlay::Rect{safe.x, safe.y, 1, 1},
                               TextSelectionOverlay::Rect{safe.right(), safe.bottom(), 1, 1}}) {
        TextSelectionOverlay::Layout layout;
        TextSelectionOverlay::placeActions(renderer, layout, point.x, point.y);
        for (const auto& button : layout.actions) {
          require(button.x >= safe.x && button.right() <= safe.right() && button.y >= safe.y &&
                      button.bottom() <= safe.bottom(),
                  "Scaled or rotated toolbar escaped usable screen bounds");
        }
        for (bool left : {false, true}) {
          const auto handle = TextSelectionOverlay::handle(safe, point, left);
          require(handle.x >= safe.x && handle.right() <= safe.right() && handle.y >= safe.y &&
                      handle.bottom() <= safe.bottom(),
                  "Selection handle escaped usable screen bounds");
        }
      }
    }
  }
  SETTINGS.uiScale = scale;
  renderer.setOrientation(orientation);
}
}  // namespace

bool ClippingSelectionSmokeTest::tick() {
  static int stage = 0;
  static uint32_t lastStep = 0;
  static size_t initialCount = 0;
  static bool pageFailureTestDone = false, blankDwellTestDone = false;
  static uint32_t blankDwellStarted = 0;
  static bool dwellJitterInjected = false;
  static bool dwellEscapeTestDone = false;
  static DictionaryClippingRequest savedCrossPageRequest;
  static DictionaryWordSelectActivity::ReaderPageLoadFn originalPageLoad = nullptr;
  static int x = 0, y = 0, initialIndex = 0;
  if (millis() - lastStep < 250 || RenderLock::peek()) return false;
  lastStep = millis();
  auto* reader = dynamic_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity());
  auto* selection = dynamic_cast<DictionaryWordSelectActivity*>(activityManager.simulatorCurrentActivity());
  const auto press = [&](const TextSelectionOverlay::Rect rect) {
    x = rect.x + rect.width / 2;
    y = rect.y + rect.height / 2;
    mappedInputManager.simulatorInjectTouchDown(x, y);
  };
  const auto selectWord = [&] {
    require(selection, "Expected text selector");
    const auto* word = selection->navigator.getSelected();
    require(word, "No selected word");
    initialIndex = selection->navigator.getCurrentFlatIndex();
    x = word->screenX + word->width / 2;
    y = word->screenY + word->heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) / 2;
    mappedInputManager.simulatorInjectTouchDown(x, y);
  };
  const auto actionsReady = [&] {
    require(selection && selection->touchSelectionActive_ && !selection->touchDragLookup_, "Missing floating actions");
    require(!selection->controller.isActive(), "Lookup began before choosing an action");
    const auto layout = selection->touchSelectionLayout();
    const auto safe = TextSelectionOverlay::safeArea(renderer);
    for (const auto& button : layout.actions) {
      require(button.x >= safe.x && button.right() <= safe.right() && button.y >= safe.y &&
                  button.bottom() <= safe.bottom(),
              "Toolbar escaped usable screen bounds");
    }
    require(layout.actions[0].y == layout.actions[1].y, "Actions are not horizontal");
  };
  const auto moveEndpoint = [&](int index) {
    require(selection && selection->touchDragLookup_, "Handle did not start a drag");
    const auto* word = selection->navigator.getWordAt(index);
    require(word, "Missing drag target");
    x = word->screenX + word->width / 2 - selection->touchDragOffsetX_;
    y = word->screenY + word->heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) / 2 -
        selection->touchDragOffsetY_;
    mappedInputManager.simulatorInjectTouchMove(x, y);
  };
  LOG_DBG("SMOKE", "Clipping selection stage %d", stage);
  switch (stage++) {
    case 0:
      require(reader, "Expected reader");
      initialCount = CLIPPINGS.clippingCount();
      reader->openWordSelect(false);
      break;
    case 1:
    case 5:
    case 17:
    case 22:
      selectWord();
      if (stage == 23) Dictionary::setLookupDictPathOverride("/dictionary-smoke/dict");
      break;
    case 2:
    case 6:
    case 9:
    case 12:
    case 15:
    case 18:
    case 20:
    case 23:
    case 25:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 3:
      actionsReady();
      require(!selection->touchLookupEnabled_, "Missing dictionary lookup enabled");
      require(selection->hasPendingClippingRequest_, "Selection range was lost");
      capture("clip-actions-no-dictionary");
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 4:
      require(selection && !selection->touchSelectionActive_, "Back did not clear selection");
      require(CLIPPINGS.clippingCount() == initialCount, "Dismissal saved a clipping");
      break;
    case 7:
      actionsReady();
      press(selection->touchSelectionLayout().endHandle);
      break;
    case 8:
      moveEndpoint(initialIndex + 3);
      break;
    case 10:
      actionsReady();
      require(selection->navigator.getLookupSelectionWordCount() == 4, "End handle did not extend the range");
      capture("clip-actions-end-handle");
      press(selection->touchSelectionLayout().startHandle);
      break;
    case 11:
      moveEndpoint(initialIndex - 2);
      break;
    case 13:
      actionsReady();
      require(selection->navigator.getLookupSelectionWordCount() == 6,
              "Start handle did not preserve opposite endpoint");
      capture("clip-actions-both-handles");
      verifyOverlayBounds();
      {
        RenderLock lock;
        auto& first = selection->workingSet_.words[initialIndex - 2];
        auto& last = selection->workingSet_.words[initialIndex + 3];
        const bool firstRtl = first.isRtl, lastRtl = last.isRtl;
        first.isRtl = last.isRtl = true;
        const auto layout = selection->touchSelectionLayout();
        require(!layout.startOnLeft && layout.endOnLeft, "RTL handles were not reversed");
        first.isRtl = firstRtl;
        last.isRtl = lastRtl;
        int oldAnchor = -1, oldCursor = -1;
        selection->navigator.getLookupSelectionRange(oldAnchor, oldCursor);
        int wrap = 0;
        while (selection->navigator.getWordAt(wrap + 1) &&
               selection->workingSet_.words[wrap].row == selection->workingSet_.words[wrap + 1].row)
          ++wrap;
        require(selection->navigator.getWordAt(wrap + 1), "Fixture has no row boundary");
        auto& prefix = selection->workingSet_.words[wrap];
        auto& suffix = selection->workingSet_.words[wrap + 1];
        const int oldNext = prefix.continuationIndex, oldPrevious = suffix.continuationOf;
        prefix.continuationIndex = wrap + 1;
        suffix.continuationOf = wrap;
        for (const int fragment : {wrap, wrap + 1}) {
          selection->navigator.setTouchSelection(fragment, fragment);
          int begin = -1, end = -1;
          require(selection->navigator.getExpandedSelectionRange(begin, end) && begin == wrap && end == wrap + 1,
                  "Wrapped selection did not include both fragments");
          const auto handles = selection->touchSelectionLayout();
          require(handles.startHandle.y ==
                          prefix.screenY + prefix.heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) &&
                      handles.endHandle.y ==
                          suffix.screenY + suffix.heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())),
                  "Wrapped word handles do not match highlighted fragments");
          DictionaryClippingRequest request;
          require(selection->captureCurrentClippingRequest(request) &&
                      request.firstPageWordOrdinal == prefix.pageWordOrdinal &&
                      request.lastPageWordOrdinal == suffix.pageWordOrdinal,
                  "Wrapped word clipping bounds differ from handles");
        }
        prefix.continuationIndex = oldNext;
        suffix.continuationOf = oldPrevious;
        selection->navigator.setTouchSelection(oldAnchor, oldCursor);
      }
      break;
    case 14:
      press(selection->touchSelectionLayout().actions[1]);
      break;
    case 16:
      if (!reader) {
        --stage;
        break;
      }
      require(CLIPPINGS.clippingCount() == initialCount + 1, "Clip was not saved exactly once");
      capture("clip-actions-saved-highlight");
      reader->openWordSelect(false);
      stage = 40;
      break;
    case 19:
      actionsReady();
      press(TextSelectionOverlay::Rect{0, 0, 2, 2});
      break;
    case 21:
      require(reader, "Outside tap did not dismiss selection");
      reader->openWordSelect(false);
      break;
    case 24:
      actionsReady();
      require(selection->touchLookupEnabled_, "Installed dictionary lookup disabled");
      capture("clip-actions-lookup");
      press(selection->touchSelectionLayout().actions[0]);
      break;
    case 40: {
      require(selection && selection->hasNextReaderPage_, "Fixture has no next reader page");
      if (!pageFailureTestDone) {
        originalPageLoad = selection->readerPageLoad_;
        selection->readerPageLoad_ = [](void*, int) -> std::unique_ptr<Page> { return nullptr; };
      }
      int last = 0;
      while (selection->navigator.getWordAt(last + 1)) ++last;
      selection->navigator.selectIndex(last - 1);
      selectWord();
      dwellJitterInjected = false;
      if (pageFailureTestDone) stage = 70;
      break;
    }
    case 70:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 71:
      actionsReady();
      press(selection->touchSelectionLayout().endHandle);
      stage = 41;
      break;
    case 41:
      if (!blankDwellTestDone) {
        const auto* word = selection->navigator.getWordAt(initialIndex + 1);
        x = word->screenX + word->width / 2;
        y = renderer.getScreenHeight() - 1;
        require(y > word->screenY + word->heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) + 8,
                "Fixture has no blank space below its last word");
        mappedInputManager.simulatorInjectTouchMove(x, y);
        blankDwellStarted = millis();
        stage = 60;
        break;
      }
      moveEndpoint(initialIndex + 1);
      break;
    case 60:
      require(selection && selection->activePageOffset_ == 0 && selection->touchPageEndIndex_ == -1,
              "Blank-space drag armed page continuation");
      if (millis() - blankDwellStarted < 1250) {
        --stage;
        break;
      }
      blankDwellTestDone = true;
      moveEndpoint(initialIndex + 1);
      stage = 42;
      break;
    case 42:
      require(selection, "Lost selector while paging");
      if (!dwellJitterInjected) {
        require(selection->touchPageEndIndex_ == initialIndex + 1, "Final word did not arm page continuation");
        const auto* word = selection->navigator.getSelected();
        // A real capacitive contact can drift into the preceding inter-word
        // gap while the user holds the final word. Stay inside the dwell slop.
        x = word->screenX - 7 - selection->touchDragOffsetX_;
        mappedInputManager.simulatorInjectTouchMove(x, y);
        dwellJitterInjected = true;
        if (pageFailureTestDone && !dwellEscapeTestDone) {
          stage = 72;
          break;
        }
        --stage;
        break;
      }
      require(selection->activePageOffset_ == 1 || selection->navigator.getCurrentFlatIndex() == initialIndex + 1,
              "Small final-word jitter cancelled the held endpoint");
      if (!pageFailureTestDone) {
        if (selection->pendingTouchPhrase_.empty()) {
          --stage;
          break;
        }
        mappedInputManager.simulatorInjectTouchRelease(x, y);
        stage = 50;
        break;
      }
      if (selection->activePageOffset_ == 0) {
        --stage;
        break;
      }
      require(selection->navigator.getCurrentFlatIndex() == 0, "Paging did not start at the first word");
      break;
    case 72:
      require(selection->navigator.getCurrentFlatIndex() == initialIndex + 1,
              "Handle dwell did not tolerate final-word jitter");
      moveEndpoint(initialIndex);
      break;
    case 73:
      require(selection->navigator.getCurrentFlatIndex() == initialIndex && selection->touchPageEndIndex_ == -1,
              "Dragging away from the final word did not cancel page continuation");
      dwellEscapeTestDone = true;
      dwellJitterInjected = false;
      moveEndpoint(initialIndex + 1);
      stage = 42;
      break;
    case 43:
      require(selection->navigator.getCurrentFlatIndex() == 0, "Stationary finger selected the whole next page");
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 44:
      actionsReady();
      require(selection->hasCrossPageSelection_, "Paging lost the first endpoint");
      require(selection->pendingClippingRequest_.firstPageOffset == 0 &&
                  selection->pendingClippingRequest_.lastPageOffset == 1,
              "Clipping request lost its page boundary");
      require(selection->touchSelectionLayout().startHandle.empty(), "Off-page start handle was drawn");
      press(selection->touchSelectionLayout().endHandle);
      break;
    case 45:
      moveEndpoint(2);
      break;
    case 46:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 47:
      actionsReady();
      require(selection->navigator.getLookupSelectionWordCount() == 3, "Next-page end handle did not update");
      capture("clip-actions-next-page");
      savedCrossPageRequest = selection->pendingClippingRequest_;
      press(selection->touchSelectionLayout().actions[1]);
      break;
    case 48:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 49: {
      if (!reader) {
        --stage;
        break;
      }
      require(CLIPPINGS.clippingCount() == initialCount + 2, "Cross-page clipping was not saved exactly once");
      const auto* clipping = CLIPPINGS.clippingAt(initialCount + 1);
      require(clipping && clipping->endPage == clipping->startPage + 1 &&
                  clipping->startWordIndex == savedCrossPageRequest.firstPageWordOrdinal &&
                  clipping->endWordIndex == savedCrossPageRequest.lastPageWordOrdinal,
              "Saved cross-page clipping lost its endpoints");
      std::string text;
      require(CLIPPINGS.readClippingText(*clipping, text) && !text.empty(), "Saved cross-page clipping has no text");
      capture("clip-actions-cross-page-saved");
      reader->openWordSelect(false);
      stage = 17;
      break;
    }
    case 50:
      actionsReady();
      press(selection->touchSelectionLayout().startHandle);
      break;
    case 51:
      moveEndpoint(initialIndex - 2);
      break;
    case 52:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 53:
      actionsReady();
      require(selection->pendingTouchPhrase_.empty(), "Handle edit retained stale fallback lookup text");
      require(selection->navigator.getLookupSelectionWordCount() == 4, "Fallback range did not follow handle edit");
      selection->readerPageLoad_ = originalPageLoad;
      pageFailureTestDone = true;
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 54:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 55:
      require(reader, "Fallback dismissal did not return to reader");
      reader->openWordSelect(false);
      stage = 40;
      break;
    case 26:
      require((selection && selection->controller.isActive()) ||
                  activityManager.isCurrentActivityNamed("DictionaryDefinition"),
              "Lookup action did not start dictionary flow");
      require(CLIPPINGS.clippingCount() == initialCount + 2, "Other actions unexpectedly saved a clipping");
      return true;
  }
  return false;
}
#endif
