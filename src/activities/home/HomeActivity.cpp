#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "companion/CompanionRenderer.h"
#include "companion/CompanionState.h"
#include "companion/CompanionTracker.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  // Resolve the calendar day once here rather than per render: the companion's
  // mood has to reflect days elapsed since the last reading session, and this
  // is the only screen that shows it outside one.
  COMPANION.refreshForDisplay();
  // Pick the line once per visit, not per render, so it stays put while the
  // menu cursor moves but is fresh every time you come back. Drawn at random
  // rather than in rotation, and never the same one twice running -- a repeat
  // reads as a bug even when it is chance. Not persisted: an SD write is not
  // worth it for flavour text, and a reshuffle after a reboot is harmless.
  static uint32_t lastQuote = UINT32_MAX;
  const uint8_t quoteCount = companion::quoteCountFor(CompanionTracker::activeId(), COMPANION.currentMood());
  if (quoteCount > 1) {
    uint32_t pick = lastQuote;
    while (pick == lastQuote) pick = esp_random() % quoteCount;
    lastQuote = pick;
    companionQuoteIndex = pick;
  } else {
    companionQuoteIndex = 0;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Consume the milestone once the user has actually been on the screen that
  // shows it. Clearing at render time instead would lose it to the very first
  // repaint, before it had been read.
  if (SETTINGS.companionEnabled && COMPANION_STATE.milestonePending) {
    COMPANION_STATE.milestonePending = false;
    COMPANION_STATE.saveToFile();
  }

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

void HomeActivity::drawCompanion(const int stripTop, const int stripBottom, const int pageWidth) const {
  if (!SETTINGS.companionEnabled || !SETTINGS.companionOnHome) return;

  constexpr int SIDE_MARGIN = 10;
  constexpr int GAP = 4;          // between the tail tip and the character
  constexpr int BUBBLE_PAD = 10;  // inside the bubble, clear of the rounded corners
  constexpr int TAIL_LENGTH = 12;
  constexpr int MIN_BUBBLE_W = 90;
  constexpr int LABEL_GAP = 2;            // between the character's feet and its status label
  constexpr int SUBLABEL_GAP = 0;         // between the mood label and the streak/progress line
  constexpr int DESCENDER_ALLOWANCE = 3;  // getTextHeight() omits descenders
  constexpr int BOTTOM_MARGIN = 2;        // keeps the last line clear of the button hints
  // Walk cycle. Advanced by redraws rather than a timer: a timer would keep the
  // panel refreshing, block the low-power idle, and accumulate e-ink ghosting.
  // Driving it from input means the character only moves when the screen was
  // going to be repainted anyway, so the motion is free.
  constexpr int WALK_STEPS = 6;
  // Kept to roughly the tail's reach: the bubble has to sit clear of the
  // character's furthest step, so every pixel of pacing pushes the bubble right.
  constexpr int WALK_TRAVEL = 14;
  constexpr int BOB_HEIGHT = 3;

  const int available = stripBottom - stripTop - BOTTOM_MARGIN;
  if (available < companion::poseHeight(1)) return;  // theme leaves no room

  // getTextHeight() reports the ascender only, but drawText() takes y as the
  // top and descenders hang below it. Without this allowance the bottom line
  // overruns into the button hints.
  const int labelH = renderer.getTextHeight(UI_10_FONT_ID) + DESCENDER_ALLOWANCE;
  const int subH = renderer.getTextHeight(SMALL_FONT_ID) + DESCENDER_ALLOWANCE;
  const int labelBlock = LABEL_GAP + labelH + SUBLABEL_GAP + subH;

  // Largest whole-pixel scale where the character and its labels both fit.
  // Fractional scaling would smear the baked dither, so it grows in whole
  // pixels only.
  int scale = 4;
  while (scale > 1 && companion::poseHeight(scale) + BOB_HEIGHT + labelBlock > available) scale--;

  const int spriteW = companion::poseWidth(scale);
  const int spriteH = companion::poseHeight(scale);
  const bool showLabel = spriteH + BOB_HEIGHT + labelBlock <= available;

  // Character and labels are centred as one block, so the status sits directly
  // under the feet instead of drifting to the bottom of the strip.
  const int blockH = spriteH + BOB_HEIGHT + (showLabel ? labelBlock : 0);
  const int blockTop = stripTop + std::max(0, (available - blockH) / 2);
  const int artHeight = spriteH + BOB_HEIGHT;

  // Ping-pong across WALK_TRAVEL, facing the direction of travel.
  const uint32_t phase = companionFrame % (WALK_STEPS * 2);
  const bool walkingBack = phase >= WALK_STEPS;
  const uint32_t step = walkingBack ? (WALK_STEPS * 2 - 1 - phase) : phase;
  const int walkX = static_cast<int>(step) * WALK_TRAVEL / (WALK_STEPS - 1);
  const int bob = (companionFrame % 2) ? BOB_HEIGHT : 0;

  const int spriteX = SIDE_MARGIN + walkX;
  const int spriteY = blockTop + bob;

  const auto id = CompanionTracker::activeId();
  const auto mood = COMPANION.currentMood();
  // A neglected companion is curled up or powered down: pacing about would
  // undercut the pose, so it stays put and only the quote rotates.
  const bool restless = mood != companion::Mood::Neglected;
  const int drawY = restless ? spriteY : spriteY - bob;
  companion::drawPose(renderer, id, mood, restless ? spriteX : SIDE_MARGIN, drawY, scale, restless && walkingBack);

  // Status lines sit centred under the character's whole pacing range, not
  // under the sprite itself, so they stay put while the character moves.
  const int lane = WALK_TRAVEL + spriteW;
  if (showLabel) {
    const char* label = companion::moodLabel(mood);

    // Second line answers "why?" and "what next?". A reachable target beats a
    // tally, so progress toward Thriving wins when there is progress to report.
    char sub[40] = "";
    const uint16_t minutes = COMPANION.minutesToday();
    const companion::MoodThresholds thresholds;
    if (minutes >= thresholds.contentMinutes && minutes < thresholds.thrivingMinutes) {
      snprintf(sub, sizeof(sub), tr(STR_COMPANION_TO_THRIVING_FORMAT), thresholds.thrivingMinutes - minutes);
    } else if (COMPANION.hasValidClock() && COMPANION_STATE.ledger.streakDays > 0) {
      snprintf(sub, sizeof(sub), tr(STR_COMPANION_STREAK_FORMAT), COMPANION_STATE.ledger.streakDays);
    }

    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    const int subW = sub[0] != '\0' ? renderer.getTextWidth(SMALL_FONT_ID, sub) : 0;

    // Both lines share one centre so they read as a block. That centre starts
    // under the character but is pushed right far enough that the widest line
    // still clears the screen edge -- "27 min to Thriving" is wider than the
    // character it sits under, so centring on the sprite alone runs it off.
    const int widest = std::max(labelW, subW);
    int centreX = SIDE_MARGIN + lane / 2;
    centreX = std::max(centreX, SIDE_MARGIN + widest / 2);
    centreX = std::min(centreX, pageWidth - SIDE_MARGIN - widest / 2);

    const int labelY = blockTop + artHeight + LABEL_GAP;
    renderer.drawText(UI_10_FONT_ID, centreX - labelW / 2, labelY, label, true, EpdFontFamily::BOLD);
    if (subW > 0) {
      renderer.drawText(SMALL_FONT_ID, centreX - subW / 2, labelY + labelH + SUBLABEL_GAP, sub);
    }
  }

  // A beaten personal best takes over the bubble once, then reverts to the
  // normal mood lines. The flag is cleared by the caller after the render so a
  // repaint mid-visit does not swallow it before it has been seen.
  const char* quote = COMPANION_STATE.milestonePending ? companion::milestoneQuoteFor(id, companionQuoteIndex)
                                                       : companion::quoteFor(id, mood, companionQuoteIndex);
  if (!quote) return;

  // Bubble body starts past the character's furthest step plus the tail, so the
  // two can never collide mid-stride.
  const int bubbleX = SIDE_MARGIN + WALK_TRAVEL + spriteW + GAP + TAIL_LENGTH;
  const int bubbleW = pageWidth - bubbleX - SIDE_MARGIN;
  if (bubbleW < MIN_BUBBLE_W) return;  // narrow screen: character only

  // Anchored to the character's band, not to the bobbing sprite, so the text
  // stays still while the character moves under it.
  const int bubbleH = std::max(40, spriteH - 8);
  const int bubbleY = blockTop + (artHeight - bubbleH) / 2;

  companion::drawSpeechBubble(renderer, bubbleX, bubbleY, bubbleW, bubbleH, TAIL_LENGTH);

  const Rect textBounds{bubbleX + BUBBLE_PAD, bubbleY + BUBBLE_PAD, bubbleW - BUBBLE_PAD * 2, bubbleH - BUBBLE_PAD * 2};
  UITheme::drawCenteredWrappedText(renderer, textBounds, UI_10_FONT_ID, quote, 3);
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (renderer.getScreenWidth() - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, metrics.contentSidePadding, coverColumnWidth, recentCount,
                                               metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      activateSelection();
    }
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  // Companion lives in the gap between the last menu row and the button hints.
  // Measured from the menu's own row height rather than a fixed offset, so it
  // adapts to themes and to the OPDS row appearing or not.
  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  // Rows are laid out at menuRowHeight + menuSpacing pitch (see the themes'
  // drawButtonMenu). getMenuRowHeight() returns only the row itself, so using it
  // here underestimates the menu by one spacing per row and the companion ends
  // up crowding the last entry. Counting the trailing spacing as well leaves a
  // natural gap between the final row and the companion.
  const int menuPitch = metrics.menuRowHeight + metrics.menuSpacing;
  const int menuBottom = menuTop + static_cast<int>(menuItems.size()) * menuPitch;
  companionFrame++;
  drawCompanion(menuBottom, pageHeight - metrics.buttonHintsHeight, pageWidth);

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
