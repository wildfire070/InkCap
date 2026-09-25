#include "Ao3ReceivedReviewActivity.h"

#include <Epub.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "../../Ao3Librarian.h"
#include "../../components/CompactHeader.h"
#include "../../components/UITheme.h"
#include "../../fontIds.h"
#include "../../util/Ao3ReceiveUtils.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"

namespace {
constexpr uint32_t MIN_FREE_HEAP = 80 * 1024;  // same floor Ao3IndexActivity uses

std::string fileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

void Ao3ReceivedReviewActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    queue = Ao3ReceiveUtils::readPending();
    total = queue.size();
    processed = 0;
    currentName.clear();
    state = State::WORKING;
  }
  requestUpdate();
}

void Ao3ReceivedReviewActivity::dropFront() {
  if (queue.empty()) return;
  Ao3ReceiveUtils::removePending(queue.front());
  RenderLock lock(*this);
  queue.erase(queue.begin());
}

// Same steps as Ao3IndexActivity's single-book mode, minus its per-book confirmation screens.
bool Ao3ReceivedReviewActivity::indexFic(const std::string& path) {
  Epub epub(path, "/.crosspoint");
  std::string publisher = epub.sniffPublisher();
  std::transform(publisher.begin(), publisher.end(), publisher.begin(), ::tolower);
  if (publisher != "archive of our own" && publisher.find("archiveofourown") == std::string::npos) return false;
  if (!epub.load(true, true, Epub::XLocationLoadMode::Skip, /*cacheCumulativeSpineSizes=*/false,
                 /*skipScraping=*/true)) {
    return false;
  }
  return Ao3Librarian::scrape(epub, /*force=*/true);
}

void Ao3ReceivedReviewActivity::processFront() {
  const std::string path = queue.front();
  if (!Storage.exists(path.c_str())) {
    dropFront();
    return;
  }

  {
    RenderLock lock(*this);
    currentName = fileNameOf(path);
    processed++;
  }
  // Show the progress screen before the blocking index work below.
  requestUpdateAndWait();

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) sdFontSystem.releaseForNetwork(renderer);
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    RenderLock lock(*this);
    state = State::MEMORY_ERROR;
    requestUpdate(true);
    return;
  }

  if (!indexFic(path)) {
    // Not an AO3 fic, or unreadable: leave the file where it is and move on.
    dropFront();
    return;
  }

  std::string workId;
  std::string title;
  {
    Epub epub(path, "/.crosspoint");
    workId = epub.getAo3WorkId();
    title = fileNameOf(path);
    auto meta = std::unique_ptr<Ao3LibraryMetadata>(new Ao3LibraryMetadata());
    if (Ao3Librarian::getLibraryInfo(epub, *meta) && meta->title[0] != '\0') title = meta->title;
  }
  std::string oldPath;
  if (workId.empty() || !Ao3Librarian::findLivePathByWorkId(workId, path, oldPath)) {
    dropFront();
    return;
  }

  askAboutDuplicate(path, oldPath, title);
}

void Ao3ReceivedReviewActivity::askAboutDuplicate(const std::string& newPath, const std::string& oldPath,
                                                  const std::string& title) {
  {
    RenderLock lock(*this);
    state = State::ASKING;
  }
  const std::string body = title + tr(STR_AO3_REVIEW_ALREADY_AT) + oldPath + tr(STR_AO3_REVIEW_REPLACE_HINT);
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_AO3_REVIEW_REPLACE_HEADING), body),
      [this, newPath, oldPath](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (Ao3ReceiveUtils::replaceExisting(oldPath, newPath)) {
            // The replaced book's index record was dropped with its cache; rebuild it now.
            indexFic(oldPath);
          } else {
            LOG_ERR("AO3R", "Replace failed: %s -> %s", newPath.c_str(), oldPath.c_str());
          }
        }
        dropFront();
        RenderLock lock(*this);
        state = State::WORKING;
        requestUpdate();
      });
}

void Ao3ReceivedReviewActivity::loop() {
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

void Ao3ReceivedReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  CompactHeader::drawTitle(renderer, tr(STR_AO3_RECEIVE));

  if (state == State::MEMORY_ERROR) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_AO3_REVIEW_NO_MEMORY),
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
  renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_AO3_REVIEW_CHECKING), true, EpdFontFamily::BOLD);
  if (!currentName.empty()) {
    const std::string line = currentName + " (" + std::to_string(processed) + "/" + std::to_string(total) + ")";
    renderer.drawCenteredText(SMALL_FONT_ID, top + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing,
                              renderer.truncatedText(SMALL_FONT_ID, line.c_str(), pageWidth - 40).c_str());
  }
  renderer.displayBuffer();
}
