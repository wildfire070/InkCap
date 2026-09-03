#include "BookDetailsActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int COVER_TO_TEXT_GAP = 16;
constexpr int ROW_V_PAD = 6;

// Draw a single "Label: value" table row; returns the y for the next row.
int drawInfoRow(GfxRenderer& renderer, int x, int y, int width, const char* label, const std::string& value) {
  // Fixed gap between label and value: trailing spaces have zero advance width in
  // the font, so we can't rely on a "Label: " space to separate them.
  constexpr int LABEL_VALUE_GAP = 6;
  const std::string lbl = std::string(label) + ":";
  const int lblWidth = renderer.getTextWidth(UI_10_FONT_ID, lbl.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, lbl.c_str(), true, EpdFontFamily::BOLD);
  const int valueX = x + lblWidth + LABEL_VALUE_GAP;
  const std::string val =
      renderer.truncatedText(UI_10_FONT_ID, value.c_str(), std::max(0, width - lblWidth - LABEL_VALUE_GAP));
  renderer.drawText(UI_10_FONT_ID, valueX, y, val.c_str());
  return y + renderer.getLineHeight(UI_10_FONT_ID) + ROW_V_PAD;
}

void drawCover(GfxRenderer& renderer, const Rect& rect, const std::string& coverPath) {
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  const auto drawFallback = [&]() {
    const char* label = tr(STR_BOOK);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - textWidth) / 2, rect.y + rect.height / 2, label, true,
                      EpdFontFamily::BOLD);
  };

  if (coverPath.empty()) {
    drawFallback();
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("BDT", coverPath, file)) {
    drawFallback();
    return;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    // rect is already sized to the chosen ratio (see pickCoverThumbWidth), so this
    // is normally a straight fill -- but the adaptive generator can still produce a
    // bitmap smaller than requested in one dimension for a cover that didn't quite
    // match even the closer of the two ratios, so center it (letterboxed) rather
    // than stretch it to the box, matching Lyra's Home cover convention.
    const int drawWidth = std::min(bitmap.getWidth(), rect.width - 4);
    const int drawHeight = std::min(bitmap.getHeight(), rect.height - 4);
    const int drawX = rect.x + (rect.width - drawWidth) / 2;
    const int drawY = rect.y + (rect.height - drawHeight) / 2;
    renderer.drawBitmap(bitmap, drawX, drawY, drawWidth, drawHeight);
  } else {
    drawFallback();
  }
  file.close();
}
}  // namespace

void BookDetailsActivity::loadMetadata() {
  // Series/rating/etc. live only in EPUB OPF/Calibre-column metadata.
  // Other formats (TXT/XTC) still get a basic details page (title/author/progress).
  RecentBook progressLookup;
  progressLookup.path = bookPath;
  progressPercent = RecentBookProgress::loadPercent(progressLookup);

  if (!FsHelpers::hasEpubExtension(bookPath)) {
    return;
  }

  Epub epub(bookPath, "/.crosspoint");
  if (!epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
    LOG_ERR("BDT", "Could not load EPUB metadata for details: %s", bookPath.c_str());
    return;
  }

  if (title.empty()) title = epub.getTitle();
  if (author.empty()) author = epub.getAuthor();
  seriesName = epub.getSeriesName();
  seriesIndex = epub.getSeriesIndex();
  contentRating = epub.getContentRating();
  completionStatus = epub.getCompletionStatus();
  chapters = epub.getChapters();
  updatedDate = epub.getUpdatedDate();
  description = epub.getDescription();

  // Cover at the Library grid's height (matches Home/RecentBooksGrid's thumbnail
  // sizing); width auto-picks between the two standard portrait ratios (3:4 or
  // 2:3) based on this book's actual cover art, same as Lyra's Home cover, and
  // letterboxes within whichever is closer for anything else (adaptive path).
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  const int coverWidth = epub.pickCoverThumbWidth(coverHeight);
  coverWidthPx = coverWidth;
  const std::string thumbPath = epub.getAdaptiveThumbBmpPath(coverWidth, coverHeight);
  if (!Storage.exists(thumbPath.c_str())) {
    epub.generateAdaptiveThumbBmp(coverWidth, coverHeight, &renderer, SETTINGS.getReaderFontId());
  }
  if (Storage.exists(thumbPath.c_str())) {
    coverPath = thumbPath;
  }
}

void BookDetailsActivity::onEnter() {
  Activity::onEnter();
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease = false;
  descScrollOffset = 0;

  // loadMetadata() can block (building a missing cache, generating the cover thumbnail).
  // Paint a "Loading…" screen synchronously first so the previous screen doesn't freeze.
  loading = true;
  requestUpdateAndWait();
  loadMetadata();
  loading = false;
  requestUpdate();
}

void BookDetailsActivity::onExit() { Activity::onExit(); }

void BookDetailsActivity::loop() {
  if (waitForBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      waitForBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

  // Page-at-a-time scroll through the description (clamped against maxScrollOffset
  // in render()); a no-op when there's no description or it all fits on screen.
  const int pageStep = std::max(1, descVisibleLines - 1);
  const auto scrollDown = [this, pageStep] {
    descScrollOffset += pageStep;
    requestUpdate();
  };
  const auto scrollUp = [this, pageStep] {
    descScrollOffset = std::max(0, descScrollOffset - pageStep);
    requestUpdate();
  };
  buttonNavigator.onNext(scrollDown);
  buttonNavigator.onPrevious(scrollUp);
  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::SwipeDir::Up:
      scrollDown();
      return;
    case MappedInputManager::SwipeDir::Down:
      scrollUp();
      return;
    default:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && Storage.exists(bookPath.c_str())) {
    onSelectBook(bookPath);
  }
}

void BookDetailsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOK_INFO), nullptr);

  if (loading) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto loadingLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, loadingLabels.btn1, loadingLabels.btn2, loadingLabels.btn3, loadingLabels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int viewportBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Cover (left), sized to the Library grid but capped so it can't crowd a short
  // (landscape) viewport off the bottom of the screen.
  int coverHeight = metrics.homeCoverHeight;
  coverHeight = std::min(coverHeight, std::max(80, viewportBottom - contentTop - 40));
  // coverWidthPx (set in loadMetadata(), from pickCoverThumbWidth) is the actual
  // ratio the generated bitmap was picked for; falls back to 3:4 for a book with
  // no cover to peek (loadMetadata() never reached the point of setting it).
  const int coverWidth = coverWidthPx > 0 ? coverWidthPx
                                          : static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 4);
  const Rect coverRect{metrics.contentSidePadding, contentTop, coverWidth, coverHeight};
  drawCover(renderer, coverRect, coverPath);

  // Metadata table (right column).
  const int textX = coverRect.x + coverRect.width + COVER_TO_TEXT_GAP;
  const int textWidth = pageWidth - textX - metrics.contentSidePadding;

  int y = contentTop + 4;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), textWidth, 2, EpdFontFamily::BOLD);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  y += ROW_V_PAD;

  // Each row is drawn only when its value is present, so missing metadata leaves no gap.
  if (!author.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_AUTHOR), author);
  if (!seriesName.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_SERIES), seriesName);
  if (!seriesIndex.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_NUMBER), seriesIndex);
  if (!contentRating.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_RATING), contentRating);
  if (!completionStatus.empty())
    y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_STATUS), completionStatus);
  if (!chapters.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_CHAPTERS), chapters);
  if (!updatedDate.empty()) y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_UPDATED), updatedDate);
  {
    const std::string progress = RecentBookProgress::hasPercent(progressPercent)
                                     ? RecentBookProgress::formatPercent(progressPercent)
                                     : std::string(tr(STR_BOOK_INFO_NOT_STARTED));
    y = drawInfoRow(renderer, textX, y, textWidth, tr(STR_BOOK_INFO_PROGRESS), progress);
  }

  // Description (full width, below cover + table). Scrollable a page at a time
  // (Up/Down or Left/Right buttons, or a vertical swipe) when it overflows.
  if (!description.empty()) {
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    int descTop = std::max(coverRect.y + coverRect.height, y) + metrics.verticalSpacing;
    if (descTop + lineH * 2 <= viewportBottom) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, descTop, tr(STR_BOOK_INFO_DESCRIPTION), true,
                        EpdFontFamily::BOLD);
      descTop += lineH + 2;
      renderer.drawLine(metrics.contentSidePadding, descTop, pageWidth - metrics.contentSidePadding, descTop);
      const int descBodyTop = descTop + ROW_V_PAD;
      const int descWidth = pageWidth - metrics.contentSidePadding * 2;
      descVisibleLines = std::max(1, (viewportBottom - descBodyTop) / lineH);

      // Wrap the full description (bounded to 4096 raw bytes upstream, so this
      // is at most a few dozen lines); only the visible window is drawn.
      constexpr int kMaxWrapLines = 500;
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, description.c_str(), descWidth, kMaxWrapLines);
      const int totalLines = static_cast<int>(lines.size());
      const int maxScrollOffset = std::max(0, totalLines - descVisibleLines);
      descScrollOffset = std::min(descScrollOffset, maxScrollOffset);

      int ly = descBodyTop;
      for (int i = descScrollOffset; i < totalLines && i < descScrollOffset + descVisibleLines; i++) {
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, ly, lines[i].c_str());
        ly += lineH;
      }

      // "more" indicator when there is content below the fold. Hand-drawn, not a
      // Unicode down-triangle/chevron glyph -- confirmed on-device that this font
      // has no such glyph (renders as a tofu box).
      if (descScrollOffset < maxScrollOffset) {
        constexpr int kMoreTriangleWidth = 10;
        constexpr int kMoreTriangleHeight = 6;
        const int triX = pageWidth - metrics.contentSidePadding - kMoreTriangleWidth;
        const int triY = viewportBottom - lineH + (lineH - kMoreTriangleHeight) / 2;
        const int xs[3] = {triX, triX + kMoreTriangleWidth, triX + kMoreTriangleWidth / 2};
        const int ys[3] = {triY, triY, triY + kMoreTriangleHeight};
        renderer.fillPolygon(xs, ys, 3);
      }
    }
  }

  const char* confirmLabel = Storage.exists(bookPath.c_str()) ? tr(STR_OPEN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
