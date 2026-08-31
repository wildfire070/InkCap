#include "CacheAllBooksActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Same 80KB floor Ao3IndexActivity's own bulk book-processing pass checks, and
// FileBrowserActivity::ensureSortCache() now checks too -- the OPF/TOC build pass
// has an unguarded allocation deep inside that can abort the whole device on low
// heap, not just fail this one book gracefully.
constexpr uint32_t kMinFreeHeapForBuild = 80 * 1024;

TouchActionButtons::Layout touchActionLayout(const GfxRenderer& renderer) {
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect screen = theme.getScreenSafeArea(renderer, true, false);
  constexpr int buttonCount = 2;
  const int totalHeight = TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap;
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}

std::string joinPath(const std::string& dirPath, const char* name) {
  return dirPath + (!dirPath.empty() && dirPath.back() == '/' ? "" : "/") + name;
}

constexpr const char* kExclusionsPath = "/.crosspoint/cache_all_books_settings.json";

}  // namespace

std::vector<std::string> CacheAllBooksActivity::loadExclusions() {
  std::vector<std::string> result;
  if (!Storage.exists(kExclusionsPath)) return result;

  String json = Storage.readFile(kExclusionsPath);
  if (json.isEmpty()) return result;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return result;

  JsonArray arr = doc["excludedFolders"];
  if (!arr.isNull()) {
    for (JsonVariant val : arr) result.push_back(val.as<std::string>());
  }
  return result;
}

void CacheAllBooksActivity::saveExclusions(const std::vector<std::string>& folders) {
  JsonDocument doc;
  JsonArray arr = doc["excludedFolders"].to<JsonArray>();
  for (const auto& folder : folders) arr.add(folder);

  String json;
  serializeJson(doc, json);
  Storage.writeFile(kExclusionsPath, json);
}

bool CacheAllBooksActivity::isExcluded(const std::string& path) const {
  for (const auto& excl : excludedFolders) {
    if (path == excl) return true;
  }
  return false;
}

void CacheAllBooksActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  excludedFolders = loadExclusions();
  requestUpdate();
}

void CacheAllBooksActivity::onExit() { Activity::onExit(); }

void CacheAllBooksActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_CACHE_ALL_BOOKS), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_CACHE_ALL_BOOKS));
  }

  if (state == WARNING) {
    // Lines 1-3 are one continuous sentence wrapped across three lines (tight,
    // even spacing); line 4 is a separate sentence, so it gets the wider gap.
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CACHE_ALL_BOOKS_INFO_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CACHE_ALL_BOOKS_INFO_2), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CACHE_ALL_BOOKS_INFO_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, tr(STR_CACHE_ALL_BOOKS_INFO_4), true);

    if (mappedInput.hasTouch()) {
      const auto actions = touchActionLayout(renderer);
      const char* labels[] = {tr(STR_CACHE_BUTTON), tr(STR_CANCEL)};
      TouchActionButtons::draw(renderer, actions, labels, 0, -1, UI_10_FONT_ID);
    } else {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CACHE_BUTTON), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == CACHING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CACHING_BOOKS));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_ALL_BOOKS_DONE), true,
                              EpdFontFamily::BOLD);
    std::string resultText = std::to_string(cachedCount) + " " + std::string(tr(STR_BOOKS_CACHED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    int nextY = pageHeight / 2 + 40;
    if (!failedNames.empty()) {
      std::string joined;
      for (size_t i = 0; i < failedNames.size(); i++) {
        if (i > 0) joined += ", ";
        joined += failedNames[i];
      }
      if (failedCount > static_cast<int>(failedNames.size())) {
        char more[32];
        snprintf(more, sizeof(more), " (+%d more)", failedCount - static_cast<int>(failedNames.size()));
        joined += more;
      }
      const int wrapWidth = std::max(1, pageWidth - metrics.contentSidePadding * 4);
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, joined.c_str(), wrapWidth, 3);
      for (const auto& line : lines) {
        renderer.drawCenteredText(UI_10_FONT_ID, nextY, line.c_str());
        nextY += lineHeight;
      }
    }
    if (lowHeapAborted) {
      renderer.drawCenteredText(UI_10_FONT_ID, nextY, tr(STR_CHECK_SERIAL_OUTPUT));
    }

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_ALL_BOOKS_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

int CacheAllBooksActivity::countEpubsRecursive(const std::string& dirPath) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return 0;

  int count = 0;
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    // Dot-prefixed entries are never worth touching here, file or directory alike,
    // regardless of the Show Hidden Files setting -- this is an internal maintenance
    // scan, not a user-facing listing. That includes macOS AppleDouble sidecar files
    // (e.g. "._Some Book.epub", written alongside the real file when a Mac copies
    // onto a non-HFS+ filesystem like this card's FAT32/exFAT) -- they match the
    // .epub extension check but aren't valid zips, so they'd otherwise show up as a
    // spurious "failed to build cache" every run. Same skip FileBrowserActivity's
    // own isMacOSMetadataEntry() already applies for its listing.
    if (name[0] == '.') {
      file.close();
      continue;
    }
    const std::string childPath = joinPath(dirPath, name);
    if (isDir) {
      // User-chosen exclusions (see isExcluded()) skip the same way -- excluding a
      // folder skips its whole subtree, since children are never discovered if it's
      // never opened.
      if (!isExcluded(childPath)) count += countEpubsRecursive(childPath);
    } else if (FsHelpers::hasEpubExtension(childPath)) {
      count++;
    }
    file.close();
  }
  dir.close();
  return count;
}

void CacheAllBooksActivity::buildCachesRecursive(const std::string& dirPath, const int total, int& processed,
                                                 bool& showingPopup, Rect& popupRect) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return;

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    // See the matching skip in countEpubsRecursive() -- dot-prefixed files
    // (AppleDouble sidecars above all) are never candidates, not just directories.
    if (name[0] == '.') {
      file.close();
      continue;
    }
    const std::string childPath = joinPath(dirPath, name);
    if (isDir) {
      if (!isExcluded(childPath)) buildCachesRecursive(childPath, total, processed, showingPopup, popupRect);
    } else if (FsHelpers::hasEpubExtension(childPath)) {
      if (!BookMetadataCache::exists(Epub::cachePathForFilePath(childPath, "/.crosspoint"))) {
        if (ESP.getFreeHeap() >= kMinFreeHeapForBuild) {
          Epub epub(childPath, "/.crosspoint");
          if (epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
            cachedCount++;
          } else {
            LOG_ERR("CACHE_ALL", "Failed to build cache: %s", childPath.c_str());
            failedCount++;
            if (failedNames.size() < kMaxFailedNamesShown) failedNames.emplace_back(name);
          }
        } else {
          LOG_ERR("CACHE_ALL", "Skipping build, %uKB free heap: %s", ESP.getFreeHeap() / 1024, childPath.c_str());
          lowHeapAborted = true;
        }
      }
      processed++;
      if (!showingPopup) {
        showingPopup = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_CACHING_BOOKS));
      }
      GUI.fillPopupProgress(renderer, popupRect, (processed * 100) / std::max(1, total));
    }
    file.close();
  }
  dir.close();
}

void CacheAllBooksActivity::cacheAllBooks() {
  cachedCount = 0;
  failedCount = 0;
  lowHeapAborted = false;
  failedNames.clear();

  // Mirrors Ao3IndexActivity::runHeapCheck(): a loaded SD custom font can be the
  // difference here, so release it and recheck before giving up. Safe to release
  // the framebuffer's font dependency here specifically because startCaching()
  // already confirmed the CACHING screen rendered synchronously before calling
  // this -- unlike Ao3IndexActivity's own check, there's no render still in flight.
  if (ESP.getFreeHeap() < kMinFreeHeapForBuild) {
    LOG_DBG("CACHE_ALL", "Free heap %u below floor, releasing SD font before retry", ESP.getFreeHeap());
    sdFontSystem.releaseForNetwork(renderer);
  }
  if (ESP.getFreeHeap() < kMinFreeHeapForBuild) {
    LOG_ERR("CACHE_ALL", "Insufficient memory to run caching (need %uKB free, have %uKB)",
            kMinFreeHeapForBuild / 1024, ESP.getFreeHeap() / 1024);
    state = FAILED;
    requestUpdate();
    return;
  }

  const int total = countEpubsRecursive("/");
  if (total > 0) {
    int processed = 0;
    bool showingPopup = false;
    Rect popupRect{0, 0, 0, 0};
    buildCachesRecursive("/", total, processed, showingPopup, popupRect);
  }

  LOG_DBG("CACHE_ALL", "Cache build complete: %d cached, %d failed", cachedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void CacheAllBooksActivity::startCaching() {
  {
    RenderLock lock(*this);
    state = CACHING;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("CACHE_ALL", "Caching screen could not be rendered synchronously; aborting cache build");
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }

  cacheAllBooks();
}

void CacheAllBooksActivity::loop() {
  if (state != CACHING && TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    goBack();
    return;
  }
  if (state == WARNING) {
    int x = 0;
    int y = 0;
    if (mappedInput.hasTouch() && mappedInput.wasScreenTouchDown(x, y)) {
      const int action = TouchActionButtons::indexAt(touchActionLayout(renderer), x, y);
      if (action == 0) {
        mappedInput.suppressNextTouchTap();
        startCaching();
        return;
      }
      if (action == 1) {
        mappedInput.suppressNextTouchTap();
        goBack();
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startCaching();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      goBack();
    }
    return;
  }
}
