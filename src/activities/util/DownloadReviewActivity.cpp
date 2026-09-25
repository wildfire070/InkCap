#include "DownloadReviewActivity.h"

#include <Epub.h>
#include <Epub/BookIds.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "BookFusionBookIdStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t MIN_FREE_HEAP = 80 * 1024;  // same floor the AO3 indexer uses

std::string fileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

void DownloadReviewActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    queue = DownloadReview::readPending();
    total = queue.size();
    processed = 0;
    currentName.clear();
    state = State::WORKING;
  }
  requestUpdate();
}

void DownloadReviewActivity::dropFront() {
  if (queue.empty()) return;
  DownloadReview::removePending(queue.front().path);
  RenderLock lock(*this);
  queue.erase(queue.begin());
}

void DownloadReviewActivity::processFront() {
  const DownloadReview::Entry entry = queue.front();
  if (!Storage.exists(entry.path.c_str())) {
    dropFront();
    return;
  }

  {
    RenderLock lock(*this);
    currentName = fileNameOf(entry.path);
    processed++;
  }
  // Show the progress screen before the blocking load below.
  requestUpdateAndWait();

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) sdFontSystem.releaseForNetwork(renderer);
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    RenderLock lock(*this);
    state = State::MEMORY_ERROR;
    requestUpdate(true);
    return;
  }

  std::string title = fileNameOf(entry.path);
  BookIds::Ids ids;
  {
    // Loading records the book's IDs (book-ids.json) as a side effect of reading its OPF.
    Epub epub(entry.path, "/.crosspoint");
    if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip)) {
      dropFront();
      return;
    }
    if (!epub.getTitle().empty()) title = epub.getTitle();
    BookIds::load(Epub::cachePathForFilePath(entry.path, "/.crosspoint"), ids);
  }
  if (ids.bookFusionId == 0) ids.bookFusionId = BookFusionBookIdStore::loadBookId(entry.path);

  std::string oldPath;
  if (!DownloadReview::findDuplicate(entry, ids, oldPath)) {
    dropFront();
    return;
  }
  askAboutDuplicate(entry.path, oldPath, title, ids.bookFusionId);
}

void DownloadReviewActivity::askAboutDuplicate(const std::string& newPath, const std::string& oldPath,
                                               const std::string& title, const uint32_t bookFusionId) {
  {
    RenderLock lock(*this);
    state = State::ASKING;
  }
  const std::string body = title + tr(STR_DUPLICATE_ALREADY_AT) + oldPath + tr(STR_DUPLICATE_REPLACE_HINT);
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DUPLICATE_REPLACE_HEADING), body),
      [this, newPath, oldPath, bookFusionId](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (DownloadReview::replaceExisting(oldPath, newPath)) {
            // The old copy is now the BookFusion book: link it so syncing follows it.
            if (bookFusionId != 0) BookFusionBookIdStore::saveBookId(oldPath, bookFusionId);
          } else {
            LOG_ERR("DLREV", "Replace failed: %s -> %s", newPath.c_str(), oldPath.c_str());
          }
        }
        dropFront();
        RenderLock lock(*this);
        state = State::WORKING;
        requestUpdate();
      });
}

void DownloadReviewActivity::loop() {
  if (state == State::MEMORY_ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      finish();  // files stay in the pending list for next time
    }
    return;
  }
  if (state != State::WORKING) return;

  if (queue.empty()) {
    finish();
    return;
  }
  processFront();
}

void DownloadReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  CompactHeader::drawTitle(renderer, tr(STR_DUPLICATE_REVIEW_TITLE));

  if (state == State::MEMORY_ERROR) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_DUPLICATE_NO_MEMORY),
                                            pageWidth - metrics.contentSidePadding * 2, 5);
    int y = pageHeight / 2 - static_cast<int>(lines.size()) * renderer.getLineHeight(UI_10_FONT_ID) / 2;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int top = pageHeight / 2 - renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_DUPLICATE_CHECKING), true, EpdFontFamily::BOLD);
  if (!currentName.empty()) {
    const std::string line = currentName + " (" + std::to_string(processed) + "/" + std::to_string(total) + ")";
    renderer.drawCenteredText(SMALL_FONT_ID, top + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing,
                              renderer.truncatedText(SMALL_FONT_ID, line.c_str(), pageWidth - 40).c_str());
  }
  renderer.displayBuffer();
}
