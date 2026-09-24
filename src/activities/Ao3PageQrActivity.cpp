#include "Ao3PageQrActivity.h"

#include <Epub.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "../Ao3LibraryMetadata.h"
#include "ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void Ao3PageQrActivity::loadMetadata() {
  storyUrl.clear();
  memset(title, 0, sizeof(title));
  memset(author, 0, sizeof(author));
  memset(tags, 0, sizeof(tags));
  memset(updatedDate, 0, sizeof(updatedDate));
  chapterCount = 0;

  const std::string cachePath = Epub::cachePathForFilePath(filePath, "/.crosspoint");
  HalFile f;
  if (Storage.openFileForRead("QRA", cachePath + "/ao3_library_info", f)) {
    Ao3LibraryMetadata meta;
    if (f.read(reinterpret_cast<uint8_t*>(&meta), sizeof(meta)) == sizeof(meta) && meta.isValid()) {
      strncpy(title, meta.title, sizeof(title) - 1);
      strncpy(author, meta.author, sizeof(author) - 1);
      for (int i = 0; i < 4; i++) strncpy(tags[i], meta.tags[i], sizeof(tags[i]) - 1);
      chapterCount = meta.chapterCount;
      strncpy(updatedDate, meta.updatedDate, sizeof(updatedDate) - 1);
    }
    f.close();
  }

  Epub epub(filePath, "/.crosspoint");
  if (epub.hasAo3Info()) {
    const std::string workId = epub.getAo3WorkId();
    if (!workId.empty()) storyUrl = "https://archiveofourown.org/works/" + workId;
  }
}

void Ao3PageQrActivity::onEnter() {
  Activity::onEnter();
  {
    // render() reads these on the render task.
    RenderLock lock(*this);
    loadMetadata();
  }
  requestUpdate();
}

void Ao3PageQrActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!filePath.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activityManager.goToReader(filePath);
  }
}

void Ao3PageQrActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AO3_PAGE_QR));

  std::vector<std::string> titleLines;
  if (title[0]) {
    titleLines = renderer.wrappedText(UI_12_FONT_ID, title, pageWidth - 40, 2, EpdFontFamily::BOLD);
  }

  std::string authorStr;
  if (author[0]) {
    authorStr = renderer.truncatedText(UI_12_FONT_ID, (std::string("by ") + author).c_str(), pageWidth - 40);
  }

  std::string infoStr = std::string(tr(STR_AO3_QR_CHAPTERS_ON_DEVICE)) + ": " + std::to_string(chapterCount);
  if (updatedDate[0]) {
    infoStr += " - " + std::string(tr(STR_AO3_QR_UPDATED_ON)) + " " + updatedDate;
  }

  // Tags are laid out no wider than the info line beneath them.
  const int infoWidth = renderer.getTextWidth(UI_10_FONT_ID, infoStr.c_str());
  const int maxTagWidth = (infoWidth > 0 && infoWidth < pageWidth - 40) ? infoWidth : (pageWidth - 40);

  int fittingCount = 0;
  int actualTotalW = 0;
  int tagWidths[4] = {};
  for (int i = 0; i < 4; i++) {
    if (!tags[i][0]) break;
    const int w = renderer.getTextWidth(SMALL_FONT_ID, tags[i]) + 16;
    const int nextW = actualTotalW + (fittingCount > 0 ? 8 : 0) + w;
    if (nextW > maxTagWidth) break;
    tagWidths[fittingCount] = w;
    actualTotalW = nextW;
    fittingCount++;
  }

  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int titleCount = static_cast<int>(titleLines.size());
  const int titleH = titleCount == 0 ? 0 : titleCount * titleLineH + (titleCount - 1) * 2;
  const int authorH = authorStr.empty() ? 0 : titleLineH;
  const int tagsH = (fittingCount > 0) ? 20 : 0;
  const int infoH = renderer.getLineHeight(UI_10_FONT_ID);

  const int gapTitleToAuthor = (titleCount > 0 && !authorStr.empty()) ? 6 : 0;
  const int gapAuthorToTags = (!authorStr.empty() && fittingCount > 0) ? 20 : 0;
  const int gapTagsToInfo = 20;

  const int subBlockH = titleH + gapTitleToAuthor + authorH + gapAuthorToTags + tagsH;
  const int topBlockH = subBlockH + gapTagsToInfo + infoH;

  constexpr int QR_SIZE = 227;
  const int qrBlockH = storyUrl.empty() ? 80 : QR_SIZE;

  const int urlH = renderer.getLineHeight(UI_10_FONT_ID);
  const int scanH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bottomBlockH = storyUrl.empty() ? 0 : (urlH + 10 + scanH);

  const int topLimit = metrics.topPadding + metrics.headerHeight;
  const int bottomLimit = pageHeight - metrics.buttonHintsHeight;
  const int availableH = bottomLimit - topLimit;
  const int totalContent = topBlockH + qrBlockH + bottomBlockH;

  const int numGaps = storyUrl.empty() ? 2 : 4;
  const int baseGap = std::max(6, (availableH - totalContent) / numGaps);
  const int gapInfoToQr = storyUrl.empty() ? baseGap : std::max(4, baseGap - 6);
  const int gapQrToBottom = std::max(4, baseGap - 6);

  const int totalBlockHeight =
      topBlockH + gapInfoToQr + qrBlockH + (storyUrl.empty() ? 0 : (gapQrToBottom + bottomBlockH));

  const int subBlockY = topLimit + std::max(0, (availableH - totalBlockHeight) / 2);
  const int infoY = subBlockY + subBlockH + gapTagsToInfo;

  int curY = subBlockY;

  if (titleCount > 0) {
    for (int i = 0; i < titleCount; i++) {
      renderer.drawCenteredText(UI_12_FONT_ID, curY, titleLines[i].c_str(), true, EpdFontFamily::BOLD);
      curY += titleLineH + (i + 1 < titleCount ? 2 : 0);
    }
    curY += gapTitleToAuthor;
  }

  if (!authorStr.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, curY, authorStr.c_str());
    curY += authorH + gapAuthorToTags;
  }

  if (fittingCount > 0) {
    int tagX = (pageWidth - actualTotalW) / 2;
    for (int i = 0; i < fittingCount; i++) {
      renderer.drawRoundedRect(tagX, curY, tagWidths[i], 20, 1, 6, true);
      renderer.drawText(SMALL_FONT_ID, tagX + 8, curY - 2, tags[i]);
      tagX += tagWidths[i] + 8;
    }
  }

  renderer.drawCenteredText(UI_10_FONT_ID, infoY, infoStr.c_str());

  curY = infoY + infoH + gapInfoToQr;

  if (!storyUrl.empty()) {
    const Rect qrBounds{(pageWidth - QR_SIZE) / 2, curY, QR_SIZE, QR_SIZE};
    QrUtils::drawQrCode(renderer, qrBounds, storyUrl);
    curY += QR_SIZE;
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, curY + 20, tr(STR_AO3_QR_NO_WORK_ID));
    renderer.drawCenteredText(SMALL_FONT_ID, curY + 48, tr(STR_AO3_QR_REINDEX_HINT));
    curY += 80;
  }

  if (!storyUrl.empty()) {
    curY += gapQrToBottom;
    const auto urlText = renderer.truncatedText(UI_10_FONT_ID, storyUrl.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, curY, urlText.c_str());
    curY += urlH + 10;
    renderer.drawCenteredText(SMALL_FONT_ID, curY, tr(STR_AO3_QR_SCAN_HINT));
  }

  const char* confirmLabel = filePath.empty() ? "" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
