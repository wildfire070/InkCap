#include "RecentBooksActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "Ao3Librarian.h"
#include "Ao3MarkedForLaterStore.h"
#include "BookActions.h"
#include "BookDetailsActivity.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "util/Ao3ArchiveUtils.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_LIST_RECENT_BOOKS = 10;
// Hold threshold for the long-press action menu (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr unsigned long ACTION_FEEDBACK_MS = 1000;
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_TAB = 2;
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RecentBooks", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), MAX_LIST_RECENT_BOOKS));

  for (const auto& book : books) {
    if (recentBooks.size() >= MAX_LIST_RECENT_BOOKS) {
      break;
    }
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::loadActiveTabEntries(const DashboardTab tab) {
  // Reassigns markedForLaterEntries/newChaptersEntries/wipsEntries/recentBooks
  // -- full vector reassignment, which can free/reallocate the backing store
  // -- while render()'s buildListScreen()/activeTabRow() reads the same
  // vectors under its own lock. Called from loop()-task sites (tab switch,
  // page navigation, post-action reload), so needs the same lock here.
  // activeTab itself is also set under this same lock (rather than by
  // callers beforehand) so render() never sees a tab index that doesn't
  // match which vector has actually been (re)loaded for it yet.
  RenderLock lock(*this);
  activeTab = tab;
  switch (activeTab) {
    case DashboardTab::MarkedForLater:
      AO3_MARKED_FOR_LATER_STORE.pruneMissing();
      markedForLaterEntries = AO3_MARKED_FOR_LATER_STORE.getEntries();
      return;
    case DashboardTab::NewChapters:
      AO3_NEW_CHAPTERS_STORE.pruneMissing();
      newChaptersEntries = AO3_NEW_CHAPTERS_STORE.getEntries();
      return;
    case DashboardTab::Wips:
      AO3_WIPS_STORE.pruneMissing();
      wipsEntries = AO3_WIPS_STORE.getEntries();
      return;
    case DashboardTab::RecentBooks:
      loadRecentBooks();
      return;
  }
}

int RecentBooksActivity::activeTabCount() const {
  switch (activeTab) {
    case DashboardTab::MarkedForLater:
      return static_cast<int>(markedForLaterEntries.size());
    case DashboardTab::NewChapters:
      return static_cast<int>(newChaptersEntries.size());
    case DashboardTab::Wips:
      return static_cast<int>(wipsEntries.size());
    case DashboardTab::RecentBooks:
      return static_cast<int>(recentBooks.size());
  }
  return 0;
}

RecentBooksActivity::DashboardRow RecentBooksActivity::activeTabRow(const size_t index) const {
  switch (activeTab) {
    case DashboardTab::MarkedForLater:
      if (index >= markedForLaterEntries.size()) return {};
      return {markedForLaterEntries[index].path, markedForLaterEntries[index].title,
              markedForLaterEntries[index].author, "#" + std::to_string(index + 1)};
    case DashboardTab::NewChapters:
      if (index >= newChaptersEntries.size()) return {};
      return {newChaptersEntries[index].path, newChaptersEntries[index].title, newChaptersEntries[index].author};
    case DashboardTab::Wips:
      if (index >= wipsEntries.size()) return {};
      return {wipsEntries[index].path, wipsEntries[index].title, wipsEntries[index].author};
    case DashboardTab::RecentBooks: {
      if (index >= recentBooks.size()) return {};
      const RecentBook& book = recentBooks[index];
      return {book.path, book.pinned ? "\xE2\x80\xA2 " + book.title : book.title, book.author};
    }
  }
  return {};
}

void RecentBooksActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<RecentBooksActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->activeTabCount())) return;
  self->selectorIndex = static_cast<size_t>(event.value);
  if (self->activeTab == DashboardTab::RecentBooks) {
    if (event.longPress) {
      self->app.clearTapFlash();
      self->showBookActionMenu(self->selectorIndex);
      return;
    }
    // Opening the book leaves this screen; a lingering flash would gray an
    // unrelated row when the list next appears.
    self->app.clearTapFlash();
    self->onSelectBook(self->recentBooks[self->selectorIndex].path);
    return;
  }
  const auto row = self->activeTabRow(self->selectorIndex);
  if (event.longPress) {
    self->app.clearTapFlash();
    self->showDashboardEntryActionMenu(row.path, row.title, row.author);
    return;
  }
  self->app.clearTapFlash();
  self->onSelectBook(row.path);
}

void RecentBooksActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<RecentBooksActivity*>(user);
  if (event.value < 0 || event.value >= TAB_COUNT) return;
  self->selectorIndex = 0;
  self->topIndex = 0;
  self->loadActiveTabEntries(static_cast<DashboardTab>(event.value));
  self->requestUpdate(true);
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned.
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // activeTab defaults to RecentBooks, so a fresh launch loads the same data
  // this screen has always shown -- the other three tabs load lazily on
  // first switch, via onTabEvent().
  loadActiveTabEntries(activeTab);

  selectorIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &RecentBooksActivity::onRowEvent, this);
  app.on(ACTION_TAB, &RecentBooksActivity::onTabEvent, this);
  app.setScreen(&RecentBooksActivity::listScreen, this);
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  markedForLaterEntries.clear();
  newChaptersEntries.clear();
  wipsEntries.clear();
}

void RecentBooksActivity::loop() {
  if (pendingCacheDeletedFeedback && millis() - cacheDeletedFeedbackShowTime >= ACTION_FEEDBACK_MS) {
    pendingCacheDeletedFeedback = false;
    requestUpdate();
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onGoHome();
    return;
  }
  const int listSize = activeTabCount();
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: open the same action menu shape used by File Browser.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (listSize > 0 && static_cast<int>(selectorIndex) < listSize &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    if (activeTab == DashboardTab::RecentBooks) {
      showBookActionMenu(selectorIndex, true);
    } else {
      const auto row = activeTabRow(selectorIndex);
      showDashboardEntryActionMenu(row.path, row.title, row.author, true);
    }
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (listSize > 0 && static_cast<int>(selectorIndex) < listSize) {
      onSelectBook(activeTabRow(selectorIndex).path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // PageBack/PageForward aren't used for anything else on this screen, so
  // they're free for tab-cycling -- short press switches tabs immediately
  // (unlike Next/Previous, whose hold variant already means "jump a page
  // within the current list", not "switch tabs").
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    selectorIndex = 0;
    topIndex = 0;
    loadActiveTabEntries(static_cast<DashboardTab>((static_cast<int>(activeTab) + 1) % TAB_COUNT));
    requestUpdate(true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    selectorIndex = 0;
    topIndex = 0;
    loadActiveTabEntries(static_cast<DashboardTab>((static_cast<int>(activeTab) + TAB_COUNT - 1) % TAB_COUNT));
    requestUpdate(true);
    return;
  }

  // Swipes scroll the viewport; the selection stays put and button navigation
  // pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, listSize);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, listSize](const int index) {
    selectorIndex = static_cast<size_t>(index);
    topIndex = followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRows, listSize);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onPreviousRelease([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onNextContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
}

void RecentBooksActivity::reloadAfterBookAction() {
  loadActiveTabEntries(activeTab);
  const int count = activeTabCount();
  if (count == 0) {
    selectorIndex = 0;
  } else if (static_cast<int>(selectorIndex) >= count) {
    selectorIndex = static_cast<size_t>(count - 1);
  }
  topIndex = followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRows, count);
  requestUpdate(true);
}

void RecentBooksActivity::promptDeleteBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }

    BookActions::clearFileMetadata(path);
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("RBA", "Failed to delete file: %s", path.c_str());
      return;
    }

    RECENT_BOOKS.removeByPath(path);
    reloadAfterBookAction();
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, title),
                         std::move(handler));
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      reloadAfterBookAction();
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title,
                                             /*ignoreInitialConfirmRelease=*/false),
      std::move(handler));
}

void RecentBooksActivity::showBookActionMenu(const size_t bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex >= recentBooks.size()) return;

  const RecentBook book = recentBooks[bookIndex];
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/true);
  if (BookActions::canSendNearby(book.path)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, book, bookIndex](const ActivityResult& result) {
        longPressFired = false;
        if (result.isCancelled) {
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("RBA", "Book action result missing");
          return;
        }

        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::BookInfo:
            openBookDetails(bookIndex);
            return;
          case FileBrowserAction::Delete:
            promptDeleteBook(book.path, book.title);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::clearBookCache(book.path)) {
                      LOG_ERR("RBA", "Failed to clear book cache for: %s", book.path.c_str());
                    } else {
                      pendingCacheDeletedFeedback = true;
                      cacheDeletedFeedbackShowTime = millis();
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(book.path)) {
                      LOG_ERR("RBA", "Failed to delete book stats for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ResetReaderSettings:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_RESET_BOOK_READER_SETTINGS),
                    book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::resetBookReaderSettings(book.path)) {
                      LOG_ERR("RBA", "Failed to reset reader settings for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_READER_SETTINGS_RESET));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ToggleCompleted: {
            bool completed = false;
            if (BookActions::toggleBookCompleted(book.path, book.title, completed)) {
              BookActions::drawToast(renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
              delay(1000);
            }
            reloadAfterBookAction();
            return;
          }
          case FileBrowserAction::EpubRenderMode: {
            const uint8_t currentIndex =
                BookActions::epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(book.path));
            startActivityForResult(
                std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "RecentEpubRenderModeSelect",
                                                          StrId::STR_EPUB_RENDER_MODE,
                                                          BookActions::epubRenderModeOptions(), currentIndex),
                [this, book](const ActivityResult& selectionResult) {
                  if (!selectionResult.isCancelled) {
                    const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
                    if (selection != nullptr &&
                        !EpubReaderActivity::saveBookRenderMode(
                            book.path, BookActions::epubRenderModeForDisplayIndex(selection->index))) {
                      LOG_ERR("RBA", "Failed to save render mode for: %s", book.path.c_str());
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          }
          case FileBrowserAction::RemoveFromRecents:
            promptRemoveBook(book.path, book.title);
            return;
          case FileBrowserAction::SendNearby:
            activityManager.goToNearbyBookSend(book.path, false);
            return;
          case FileBrowserAction::PinToHome:
            if (!RECENT_BOOKS.setPinned(book.path, true)) {
              RenderLock lock(*this);
              BookActions::drawToast(renderer, tr(STR_PIN_LIMIT_REACHED));
            }
            reloadAfterBookAction();
            return;
          case FileBrowserAction::UnpinFromHome:
            RECENT_BOOKS.setPinned(book.path, false);
            reloadAfterBookAction();
            return;
          case FileBrowserAction::MarkForLater:
            AO3_MARKED_FOR_LATER_STORE.addBook(book.path, book.title, book.author);
            reloadAfterBookAction();
            return;
          case FileBrowserAction::UnmarkForLater:
            AO3_MARKED_FOR_LATER_STORE.removeByPath(book.path);
            reloadAfterBookAction();
            return;
          case FileBrowserAction::ArchiveFic:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ARCHIVE_CONFIRM_HEADING),
                                                        tr(STR_ARCHIVE_CONFIRM_BODY)),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled &&
                      Ao3ArchiveUtils::archiveFic(book.path, book.title, book.author).empty()) {
                    RenderLock lock(*this);
                    BookActions::drawToast(renderer, tr(STR_ERROR_GENERAL_FAILURE));
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::RestoreFic:
            if (Ao3ArchiveUtils::restoreFic(book.path).empty()) {
              RenderLock lock(*this);
              BookActions::drawToast(renderer, tr(STR_ERROR_GENERAL_FAILURE));
            }
            reloadAfterBookAction();
            return;
          case FileBrowserAction::PinFavorite:
          case FileBrowserAction::UnpinFavorite:
          case FileBrowserAction::PinBootFavorite:
          case FileBrowserAction::UnpinBootFavorite:
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::ViewBookmarks:
          case FileBrowserAction::ViewClippings:
          case FileBrowserAction::DeleteBookmarks:
          case FileBrowserAction::DeleteClippings:
            return;
        }
      });
}

void RecentBooksActivity::openBookDetails(const size_t bookIndex) {
  if (bookIndex >= recentBooks.size()) return;
  const RecentBook& book = recentBooks[bookIndex];
  const bool hasPrevious = bookIndex > 0;
  const bool hasNext = bookIndex + 1 < recentBooks.size();
  startActivityForResult(
      std::make_unique<BookDetailsActivity>(renderer, mappedInput, book.path, book.title, book.author, hasPrevious,
                                            hasNext),
      [this, bookIndex, hasPrevious, hasNext](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (const auto* nav = std::get_if<BookDetailsNavResult>(&result.data)) {
            if (nav->next && hasNext) {
              openBookDetails(bookIndex + 1);
              return;
            }
            if (!nav->next && hasPrevious) {
              openBookDetails(bookIndex - 1);
              return;
            }
          }
        }
        reloadAfterBookAction();
      });
}

void RecentBooksActivity::showDashboardEntryActionMenu(const std::string& path, const std::string& title,
                                                       const std::string& author,
                                                       const bool ignoreInitialConfirmRelease) {
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(path, /*includeRemoveFromRecents=*/false);
  if (BookActions::canSendNearby(path)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, title, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, path, title, author](const ActivityResult& result) {
        longPressFired = false;
        if (result.isCancelled) {
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("RBA", "Dashboard entry action result missing");
          return;
        }

        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::BookInfo:
            // No Prev/Next chaining here -- unlike the Recent Books tab, these
            // rows aren't a stable index-addressable list the user is paging
            // through (WIPs is alphabetical, Marked/New Chapters are FIFO/MRU
            // and can reorder on any store mutation elsewhere).
            startActivityForResult(
                std::make_unique<BookDetailsActivity>(renderer, mappedInput, path, title, author,
                                                      /*hasPrevious=*/false, /*hasNext=*/false),
                [this](const ActivityResult&) { reloadAfterBookAction(); });
            return;
          case FileBrowserAction::Delete:
            promptDeleteBook(path, title);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), title),
                [this, path](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::clearBookCache(path)) {
                      LOG_ERR("RBA", "Failed to clear book cache for: %s", path.c_str());
                    } else {
                      pendingCacheDeletedFeedback = true;
                      cacheDeletedFeedbackShowTime = millis();
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS), title),
                [this, path](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(path)) {
                      LOG_ERR("RBA", "Failed to delete book stats for: %s", path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ResetReaderSettings:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_RESET_BOOK_READER_SETTINGS),
                    title),
                [this, path](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::resetBookReaderSettings(path)) {
                      LOG_ERR("RBA", "Failed to reset reader settings for: %s", path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_READER_SETTINGS_RESET));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ToggleCompleted: {
            bool completed = false;
            if (BookActions::toggleBookCompleted(path, title, completed)) {
              BookActions::drawToast(renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
              delay(1000);
            }
            reloadAfterBookAction();
            return;
          }
          case FileBrowserAction::EpubRenderMode: {
            const uint8_t currentIndex =
                BookActions::epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(path));
            startActivityForResult(
                std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "DashboardEpubRenderModeSelect",
                                                          StrId::STR_EPUB_RENDER_MODE,
                                                          BookActions::epubRenderModeOptions(), currentIndex),
                [this, path](const ActivityResult& selectionResult) {
                  if (!selectionResult.isCancelled) {
                    const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
                    if (selection != nullptr &&
                        !EpubReaderActivity::saveBookRenderMode(
                            path, BookActions::epubRenderModeForDisplayIndex(selection->index))) {
                      LOG_ERR("RBA", "Failed to save render mode for: %s", path.c_str());
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          }
          case FileBrowserAction::SendNearby:
            activityManager.goToNearbyBookSend(path, false);
            return;
          case FileBrowserAction::PinToHome:
            if (!RECENT_BOOKS.setPinned(path, true)) {
              RenderLock lock(*this);
              BookActions::drawToast(renderer, tr(STR_PIN_LIMIT_REACHED));
            }
            reloadAfterBookAction();
            return;
          case FileBrowserAction::UnpinFromHome:
            RECENT_BOOKS.setPinned(path, false);
            reloadAfterBookAction();
            return;
          case FileBrowserAction::MarkForLater:
            AO3_MARKED_FOR_LATER_STORE.addBook(path, title, author);
            reloadAfterBookAction();
            return;
          case FileBrowserAction::UnmarkForLater:
            AO3_MARKED_FOR_LATER_STORE.removeByPath(path);
            reloadAfterBookAction();
            return;
          case FileBrowserAction::ArchiveFic:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ARCHIVE_CONFIRM_HEADING),
                                                        tr(STR_ARCHIVE_CONFIRM_BODY)),
                [this, path, title, author](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled && Ao3ArchiveUtils::archiveFic(path, title, author).empty()) {
                    RenderLock lock(*this);
                    BookActions::drawToast(renderer, tr(STR_ERROR_GENERAL_FAILURE));
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::RestoreFic:
            if (Ao3ArchiveUtils::restoreFic(path).empty()) {
              RenderLock lock(*this);
              BookActions::drawToast(renderer, tr(STR_ERROR_GENERAL_FAILURE));
            }
            reloadAfterBookAction();
            return;
          case FileBrowserAction::PinFavorite:
          case FileBrowserAction::UnpinFavorite:
          case FileBrowserAction::PinBootFavorite:
          case FileBrowserAction::UnpinBootFavorite:
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::ViewBookmarks:
          case FileBrowserAction::ViewClippings:
          case FileBrowserAction::DeleteBookmarks:
          case FileBrowserAction::DeleteClippings:
          case FileBrowserAction::RemoveFromRecents:
            return;
        }
      });
}

void RecentBooksActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<RecentBooksActivity*>(user)->buildListScreen(screen);
}

void RecentBooksActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  // Dashboard tab bar -- simplified from SettingsActivity's own tab bar (no
  // RoundedRaff-specific pill styling or focused/unfocused dimming: unlike
  // there, selectorIndex here always addresses a list row, never the tab
  // band itself, so there's no "focus moved to the tabs" state to distinguish).
  fui::TabItem tabs[TAB_COUNT];
  tabs[0].label = tr(STR_TAB_MARKED_FOR_LATER);
  tabs[0].value = static_cast<int16_t>(DashboardTab::MarkedForLater);
  tabs[0].selected = activeTab == DashboardTab::MarkedForLater;
  tabs[1].label = tr(STR_TAB_NEW_CHAPTERS);
  tabs[1].value = static_cast<int16_t>(DashboardTab::NewChapters);
  tabs[1].selected = activeTab == DashboardTab::NewChapters;
  tabs[2].label = tr(STR_TAB_WIPS);
  tabs[2].value = static_cast<int16_t>(DashboardTab::Wips);
  tabs[2].selected = activeTab == DashboardTab::Wips;
  tabs[3].label = tr(STR_RECENTS);
  tabs[3].value = static_cast<int16_t>(DashboardTab::RecentBooks);
  tabs[3].selected = activeTab == DashboardTab::RecentBooks;

  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = TAB_COUNT;
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = fui::InputTouch;
  tabProps.text = screen.theme().smallText;
  tabProps.divider = true;
  const int16_t tabLineHeight = screen.target().lineHeight(screen.theme().smallText.font);
  constexpr int16_t kTouchTabBarHeight = 50;
  const int16_t preferredTabHeight =
      mappedInput.hasTouch() ? kTouchTabBarHeight : static_cast<int16_t>(metrics.tabBarHeight);
  const int16_t tabBand = preferredTabHeight > tabLineHeight + 10 ? preferredTabHeight : tabLineHeight + 10;
  const fui::Rect tabRect = screen.takeTop(tabBand);
  drawUiTabBar(screen, tabProps, tabRect, metrics.tabBarAppearance);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = activeTabCount();
  if (count == 0) {
    const StrId emptyStrId = activeTab == DashboardTab::MarkedForLater  ? StrId::STR_NO_MARKED_FOR_LATER
                             : activeTab == DashboardTab::NewChapters   ? StrId::STR_NO_NEW_CHAPTERS
                             : activeTab == DashboardTab::Wips          ? StrId::STR_NO_WIPS
                                                                        : StrId::STR_NO_RECENT_BOOKS;
    screen.centeredText(I18n::getInstance().get(emptyStrId), screen.theme().bodyText);
    return;
  }

  // Transient per-render: points into whichever tab's own entry vector.
  // itemStrings keeps the projected (title, author) pairs alive for the
  // duration of this call, since fui::ListItem only stores pointers.
  std::vector<DashboardRow> itemStrings;
  itemStrings.reserve(static_cast<size_t>(count));
  std::vector<fui::ListItem> items;
  items.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    itemStrings.push_back(activeTabRow(static_cast<size_t>(i)));
    const auto& row = itemStrings.back();
    fui::ListItem item;
    item.label = row.title.c_str();
    if (!row.author.empty()) item.subtitle = row.author.c_str();
    if (!row.value.empty()) item.value = row.value.c_str();  // queue position, MarkedForLater tab only
    const UIIcon fileIcon = UITheme::getFileIcon(row.path);
    const BookStatus status = fileIcon == UIIcon::Book
                                  ? Ao3Librarian::getBookStatus(Epub::cachePathForFilePath(row.path, "/.crosspoint"))
                                  : BookStatus::START;
    // Always checked, not just on the MarkedForLater tab itself -- a book
    // showing in Recents/NewChapters/Wips can also be marked independently.
    const bool markedForLater = AO3_MARKED_FOR_LATER_STORE.contains(row.path);
    item.icon = listIconForBookStatus(fileIcon, status, markedForLater, 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(items.size());
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);  // physical buttons stay in loop()
  props.iconSize = 28;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  const fui::Rect listBounds = screen.body();
  const auto rows = configureUiList(props, screen.theme(), listBounds, UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, count);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_MENU_RECENT_BOOKS), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_MENU_RECENT_BOOKS));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_HOME)), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (pendingCacheDeletedFeedback) {
    GUI.drawPopup(renderer, tr(STR_BOOK_CACHE_DELETED));
  }

  renderer.displayBuffer();
}
