#include "Ao3PageQrActivity.h"

#include <Epub.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "../Ao3LibraryMetadata.h"
#include "ActivityManager.h"
#include "activities/network/Ao3ReceiveActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
// TouchRegistry::Item ids for the explicit touch buttons render() draws in place of the
// button-hints row (see the touch-handling comment in loop()).
constexpr int OPEN_BUTTON_ID = 0;
constexpr int UPDATE_BUTTON_ID = 1;

// Matches the wording Ao3LibraryActivity's Sort & Filter overlay already uses for this
// same rating char, so the two screens describe a rating the same way.
const char* ratingLabel(const char rating) {
  switch (rating) {
    case 'G':
      return "General";
    case 'T':
      return "Teen";
    case 'M':
      return "Mature";
    case 'E':
      return "Explicit";
    default:
      return "Not Rated";
  }
}
}  // namespace

void Ao3PageQrActivity::loadMetadata() {
  storyUrl.clear();
  memset(title, 0, sizeof(title));
  memset(author, 0, sizeof(author));
  memset(tags, 0, sizeof(tags));
  memset(updatedDate, 0, sizeof(updatedDate));
  rating = 0;
  chapterCount = 0;

  const std::string cachePath = Epub::cachePathForFilePath(filePath, "/.crosspoint");
  HalFile f;
  if (Storage.openFileForRead("QRA", cachePath + "/ao3_library_info", f)) {
    Ao3LibraryMetadata meta;
    if (f.read(reinterpret_cast<uint8_t*>(&meta), sizeof(meta)) == sizeof(meta) && meta.isValid()) {
      strncpy(title, meta.title, sizeof(title) - 1);
      strncpy(author, meta.author, sizeof(author) - 1);
      for (int i = 0; i < 4; i++) strncpy(tags[i], meta.tags[i], sizeof(tags[i]) - 1);
      rating = meta.rating;
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
    return;
  }

  // Touch (X4 Pro): the board has no Left/Confirm keys, and BaseTheme::drawButtonHints() -- the
  // usual touch fallback for a button-hint label -- no-ops entirely on touch hardware (it only
  // draws the physical-button hint row), so this screen's Open and Update actions each need their
  // own explicit touch target. render() draws and registers each only when its action applies
  // (filePath/storyUrl non-empty respectively), same gates used here.
  int tappedItem = -1;
  const bool itemTapped = mappedInput.wasItemTapped(tappedItem);
  const bool openTapped = !filePath.empty() && itemTapped && tappedItem == OPEN_BUTTON_ID;
  const bool updateTapped = !storyUrl.empty() && itemTapped && tappedItem == UPDATE_BUTTON_ID;
  if (openTapped || updateTapped) mappedInput.suppressCurrentTouchContact();

  if (!filePath.empty() && openTapped) {
    activityManager.goToReader(filePath);
    return;
  }

  if (!storyUrl.empty() && (mappedInput.wasReleased(MappedInputManager::Button::Left) || updateTapped)) {
    startActivityForResult(
        std::make_unique<Ao3ReceiveActivity>(renderer, mappedInput, Ao3ReceiveMode::UPDATE_SINGLE, filePath,
                                             storyUrl, std::string(title), std::string(author)),
        [this](const ActivityResult&) { requestUpdate(true); });
  }
}

void Ao3PageQrActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Touch (X4 Pro): metrics.buttonHintsHeight is 0 on touch hardware (BaseTheme reserves that
  // band only for the physical-button hint row it draws, which it skips on touch -- see
  // BaseTheme::drawButtonHints()'s early `if (gpio.hasTouch()) return;`), so the content layout
  // below would otherwise use the full page height and leave no room for the explicit touch
  // Open/Update buttons drawn at the end of this function. Reserve the same height for both.
  constexpr int TOUCH_FOOTER_BUTTON_HEIGHT = 48;
  const bool hasOpenAction = !filePath.empty();
  const bool hasUpdateAction = !storyUrl.empty();
  const int touchFooterButtonCount = (hasOpenAction ? 1 : 0) + (hasUpdateAction ? 1 : 0);
  const int touchButtonReserve =
      mappedInput.hasTouchHardware() ? touchFooterButtonCount * TOUCH_FOOTER_BUTTON_HEIGHT : 0;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AO3_PAGE_QR));

  std::vector<std::string> titleLines;
  if (title[0]) {
    titleLines = renderer.wrappedText(UI_12_FONT_ID, title, pageWidth - 40, 2, EpdFontFamily::BOLD);
  }

  std::string authorStr;
  if (author[0]) {
    authorStr = renderer.truncatedText(UI_12_FONT_ID, (std::string("by ") + author).c_str(), pageWidth - 40);
  }

  const std::string infoLine1 = std::string(tr(STR_AO3_QR_CHAPTERS_ON_DEVICE)) + ": " + std::to_string(chapterCount);
  std::string infoLine2;
  if (updatedDate[0]) {
    infoLine2 = std::string(tr(STR_AO3_QR_UPDATED_ON)) + " " + updatedDate;
  }

  // Tags are laid out no wider than the chapter-count line beneath them -- but only when
  // that's the only info line. Once an "Updated on" second line exists, there's no single
  // line left to visually match against, so tags simply use the full available width
  // (matching how this already behaved before the two-line split, back when the combined
  // line was wide enough to overflow and fall back to the same full-width case below).
  const int infoWidth1 = renderer.getTextWidth(UI_10_FONT_ID, infoLine1.c_str());
  const int maxTagWidth = (infoLine2.empty() && infoWidth1 > 0 && infoWidth1 < pageWidth - 40)
                               ? infoWidth1
                               : (pageWidth - 40);

  // Pill 0 is always the rating; pills 1-2 are the first two scraped tags (same freeform
  // "Genre:"/"Additional Tags:" values as before -- just two of them now, to make room).
  std::string pillTexts[3] = {ratingLabel(rating)};
  int pillTextCount = 1;
  for (int i = 0; i < 2 && tags[i][0]; i++) pillTexts[pillTextCount++] = tags[i];

  int fittingCount = 0;
  int actualTotalW = 0;
  int tagWidths[3] = {};
  for (int i = 0; i < pillTextCount; i++) {
    const int w = renderer.getTextWidth(SMALL_FONT_ID, pillTexts[i].c_str()) + 16;
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
  const int infoLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int infoH = infoLine2.empty() ? infoLineH : (infoLineH * 2 + 2);

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
  const int bottomLimit = pageHeight - metrics.buttonHintsHeight - touchButtonReserve;
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
      renderer.drawText(SMALL_FONT_ID, tagX + 8, curY - 2, pillTexts[i].c_str());
      tagX += tagWidths[i] + 8;
    }
  }

  const auto truncInfoLine1 = renderer.truncatedText(UI_10_FONT_ID, infoLine1.c_str(), pageWidth - 40);
  renderer.drawCenteredText(UI_10_FONT_ID, infoY, truncInfoLine1.c_str());
  if (!infoLine2.empty()) {
    const auto truncInfoLine2 = renderer.truncatedText(UI_10_FONT_ID, infoLine2.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, infoY + infoLineH + 2, truncInfoLine2.c_str());
  }

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
  const char* updateLabel = storyUrl.empty() ? "" : tr(STR_AO3_QR_UPDATE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, updateLabel, "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Touch (X4 Pro): drawButtonHints() above no-ops on touch hardware (it only draws the
  // physical-button hint row), leaving its reserved bottom band empty and the Open/Update actions
  // with no touch target at all -- see loop()'s matching tap checks. Draw explicit button(s)
  // filling that same reserved band instead, via the shared TouchActionButtons helper (same
  // render-time Layout used for both drawing and TouchRegistry hit-testing, so the two can't
  // drift apart -- see BackupStatsActivity for the same pattern). Open and Update each get their
  // own row, stacked, when both apply; otherwise whichever one applies takes the one row.
  if (touchButtonReserve > 0) {
    const Rect container{0, pageHeight - touchButtonReserve, pageWidth, touchButtonReserve};
    const auto actions =
        TouchActionButtons::vertical(container, static_cast<uint8_t>(touchFooterButtonCount), TOUCH_FOOTER_BUTTON_HEIGHT, 0);
    const char* actionLabels[2] = {};
    int labelCount = 0;
    if (hasOpenAction) actionLabels[labelCount++] = tr(STR_OPEN);
    if (hasUpdateAction) actionLabels[labelCount++] = tr(STR_AO3_QR_UPDATE);
    TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
    for (uint8_t i = 0; i < actions.count; i++) {
      const int id = (i == 0 && hasOpenAction) ? OPEN_BUTTON_ID : UPDATE_BUTTON_ID;
      TouchRegistry::getInstance().add(actions.buttons[i], id, TouchRegistry::Item);
    }
  }

  renderer.displayBuffer();
}
