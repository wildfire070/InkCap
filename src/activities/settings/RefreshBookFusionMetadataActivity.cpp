#include "RefreshBookFusionMetadataActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "BookFusionBookIdStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookFusionCoverCache.h"
#include "network/WifiUtils.h"
#include "util/LibraryScan.h"

namespace {
// Matches Epub's own kDefaultThumbHeight -- same constant BookFusionBrowserActivity
// uses when downloading a cover for the first time.
constexpr int COVER_THUMB_HEIGHT = 180;
// searchBooks(list=nullptr) walks every book the account has, in pages of
// BOOKFUSION_BOOKS_PER_PAGE; cap the walk so a very large library can't spin
// forever if every local book somehow fails to match.
constexpr int MAX_PAGES = 500;
}  // namespace

void RefreshBookFusionMetadataActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  state = WARNING;
  statusMessage.clear();
  errorMessage.clear();
  localBookPaths.clear();
  totalLocal = matched = coversOk = positionsOk = failed = 0;
  requestUpdate();
}

void RefreshBookFusionMetadataActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

void RefreshBookFusionMetadataActivity::returnToCaller() { finish(); }

void RefreshBookFusionMetadataActivity::loop() {
  if (state == WIFI_SELECTION) return;

  if (state == WARNING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (BookFusionSyncClient::getBearerToken().empty()) {
        state = ERROR;
        errorMessage = tr(STR_BF_NO_TOKEN_MSG);
        requestUpdate();
        return;
      }
      state = CONNECTING;
      statusMessage = tr(STR_CHECKING_WIFI);
      requestUpdate();
      if (hasActiveStationWifiConnection()) {
        onWifiSelectionComplete(true);
      } else {
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state == CONNECTING || state == RUNNING) return;

  if (state == SUCCESS || state == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToCaller();
    }
  }
}

void RefreshBookFusionMetadataActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    state = ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  state = RUNNING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BFRefresh", "Running screen could not be rendered before scan");
    requestUpdate(true);
  }

  sdFontSystem.releaseForNetwork(renderer);
  runRefresh();
}

void RefreshBookFusionMetadataActivity::runRefresh() {
  // Whole-SD scan is itself blocking and can take a few seconds; the
  // "Running" screen is already up from onWifiSelectionComplete(). Filtered
  // directly during the scan rather than via an intermediate full path
  // list -- most books won't have a BookFusion sidecar, so this keeps peak
  // memory down to just the (usually much smaller) linked subset.
  localBookPaths.clear();
  LibraryScan::enumerateBooks([this](const std::string& path) {
    if (BookFusionBookIdStore::loadBookId(path) != 0) {
      localBookPaths.push_back(path);
    }
  });
  totalLocal = static_cast<int>(localBookPaths.size());

  if (totalLocal == 0) {
    state = SUCCESS;
    statusMessage = tr(STR_BF_REFRESH_NONE_LINKED);
    requestUpdate();
    return;
  }

  int remaining = totalLocal;
  for (int page = 1; page <= MAX_PAGES && remaining > 0; page++) {
    BookFusionSearchResult result;
    renderer.releaseFrameBuffersForNetwork();
    const auto err = BookFusionSyncClient::searchBooks(page, nullptr, result);
    if (!renderer.reallocFrameBuffersAfterNetwork()) {
      LOG_ERR("BFRefresh", "Framebuffer realloc failed after network fetch");
      ESP.restart();
    }

    if (err != BookFusionSyncClient::OK) {
      if (matched == 0) {
        state = ERROR;
        errorMessage = BookFusionSyncClient::errorString(err);
        requestUpdate();
        return;
      }
      break;  // Made some progress; report what succeeded instead of failing the whole run.
    }

    for (const auto& remoteBook : result.books) {
      if (remaining == 0) break;
      for (auto it = localBookPaths.begin(); it != localBookPaths.end(); ++it) {
        if (BookFusionBookIdStore::loadBookId(*it) != remoteBook.bookId) continue;
        matched++;
        remaining--;
        refreshOneBook(remoteBook, *it);

        statusMessage = tr(STR_LOADING);
        char detail[64];
        snprintf(detail, sizeof(detail), "%d/%d", matched, totalLocal);
        statusMessage += " ";
        statusMessage += detail;
        requestUpdate(true);

        localBookPaths.erase(it);
        break;
      }
    }

    if (!result.hasMore) break;
  }

  state = SUCCESS;
  char summary[96];
  snprintf(summary, sizeof(summary), tr(STR_BF_REFRESH_DETAIL), matched, totalLocal, coversOk, positionsOk);
  statusMessage = summary;
  requestUpdate();
}

void RefreshBookFusionMetadataActivity::refreshOneBook(const BookFusionBook& book, const std::string& localPath) {
  Epub epub(localPath, "/.crosspoint");
  epub.setupCacheDir();

  if (!book.coverUrl.empty()) {
    GfxRenderer::NetworkBufferLoan fbLoan(renderer);
    if (BookFusionCoverCache::download(book.coverUrl, epub) && BookFusionCoverCache::convert(epub, COVER_THUMB_HEIGHT)) {
      coversOk++;
    } else {
      failed++;
    }
  }

  // Never clobber a position the user has actually established locally.
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  if (!Storage.exists(progressPath.c_str())) {
    BookFusionProgress remote;
    GfxRenderer::NetworkBufferLoan fbLoan(renderer);
    if (BookFusionSyncClient::getProgress(book.bookId, remote) == BookFusionSyncClient::OK && remote.percentage > 0.0f) {
      int spineIndex = 0;
      float spineProgress = 0.0f;
      bool resolved = false;
      if (remote.hasChapterInfo && remote.chapterIndex >= 0 && remote.chapterIndex < epub.getSpineItemsCount()) {
        spineIndex = remote.chapterIndex;
        resolved = true;
      } else {
        const int percentInt = std::max(0, std::min(100, static_cast<int>(remote.percentage * 100.0f + 0.5f)));
        resolved = epub.resolveLocationPercentToSpineProgress(percentInt, spineIndex, spineProgress);
      }
      bool saved = false;
      if (resolved) {
        // The chapter's real page count is unknown without opening the book, so save a
        // fixed-precision placeholder approximating spineProgress rather than page 0 of 1 --
        // loadProgress()'s (page+1)/pageCount math would read page 0/1 back as 100% through the
        // chapter (not "just starting it"), showing an inflated in-progress percentage anywhere
        // that reads this book's saved progress before it's ever actually opened.
        constexpr int kPlaceholderPageCount = 10000;
        const int placeholderPage = std::max(
            0, std::min(kPlaceholderPageCount - 1, static_cast<int>(spineProgress * kPlaceholderPageCount)));
        saved = EpubReaderUtils::saveProgress(epub, spineIndex, placeholderPage, kPlaceholderPageCount);
      }
      if (saved) positionsOk++;
    }
  }
}

void RefreshBookFusionMetadataActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  GUI.drawHeader(renderer, header, tr(STR_BF_REFRESH_METADATA));

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineH) / 2;

  switch (state) {
    case WARNING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BF_REFRESH_WARNING));
      break;
    case CONNECTING:
    case RUNNING:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
      break;
    case SUCCESS:
      renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
      break;
    case ERROR:
    case WIFI_SELECTION:
      renderer.drawCenteredText(UI_10_FONT_ID, top, errorMessage.c_str());
      break;
  }

  MappedInputManager::Labels labels;
  if (state == WARNING || state == SUCCESS || state == ERROR) {
    labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  } else {
    labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
