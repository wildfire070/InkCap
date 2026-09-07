#include "BookActionActivity.h"

#include <Epub.h>
#include <HalStorage.h>
#include <I18n.h>

#include "../../Ao3Librarian.h"
#include "../../Ao3MarkedForLaterStore.h"
#include "../../components/UITheme.h"
#include "../../util/Ao3ArchiveUtils.h"
#include "../util/ConfirmationActivity.h"
#include "Ao3IndexActivity.h"
#include "BookActions.h"

BookActionActivity::BookActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                                       std::string fileName)
    : Activity("BookAction", renderer, mappedInput), filePath(std::move(filePath)), fileName(std::move(fileName)) {}

void BookActionActivity::onEnter() {
  Activity::onEnter();

  // Load current status
  std::string cachePath = Epub::cachePathForFilePath(filePath, "/.crosspoint");
  currentStatus = Ao3Librarian::getBookStatus(cachePath);
  initialStatus = currentStatus;

  hasAo3LibraryInfo = Storage.exists((cachePath + "/ao3_library_info").c_str());
  bookIsArchived = Ao3ArchiveUtils::isArchived(filePath);

  requestUpdate(true);
}

void BookActionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 fileName.c_str());

  auto rowTitle = [this](int index) {
    switch (index) {
      case 0:
        return std::string("Book Status: ") + getStatusLabel(currentStatus);
      case 1:
        return hasAo3LibraryInfo ? std::string("Reindex Book") : std::string("Index Book");
      case 2:
        return std::string(AO3_MARKED_FOR_LATER_STORE.contains(filePath) ? tr(STR_UNMARK_FOR_LATER)
                                                                         : tr(STR_MARK_FOR_LATER));
      case 3:
        return std::string(bookIsArchived ? tr(STR_RESTORE_FIC) : tr(STR_ARCHIVE_FIC));
      default:
        return std::string(tr(STR_DELETE));
    }
  };

  // Surfaces how close "Mark for Later" is to its cap, rather than letting it
  // silently no-op once full with no visible warning.
  auto rowValue = [this](int index) {
    if (index == 2) {
      return std::to_string(AO3_MARKED_FOR_LATER_STORE.getCount()) + "/" +
             std::to_string(Ao3MarkedForLaterStore::MAX_ENTRIES);
    }
    return std::string();
  };

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, renderer.getScreenWidth(),
           renderer.getScreenHeight() - metrics.headerHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2},
      ROW_COUNT, selectorIndex, rowTitle, nullptr, nullptr, rowValue);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BookActionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (currentStatus != initialStatus || markedForLaterChanged || wasRestored) {
      if (currentStatus != initialStatus) saveStatus();
      BookActionResult res;
      res.modified = true;
      res.newStatus = currentStatus;
      res.markedForLaterChanged = markedForLaterChanged;
      res.restored = wasRestored;
      setResult(ActivityResult(std::move(res)));
    }
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == 0) {
      // Cycle status
      uint8_t s = static_cast<uint8_t>(currentStatus);
      s = (s + 1) % 5;
      currentStatus = static_cast<BookStatus>(s);
      requestUpdate(true);
    } else if (selectorIndex == 1) {
      // Launch Ao3IndexActivity in SINGLE mode
      auto handler = [this](const ActivityResult& res) {
        if (const auto* indexRes = std::get_if<Ao3IndexResult>(&res.data)) {
          if (indexRes->successfullyIndexed) {
            BookActionResult result;
            result.modified = true;
            result.indexingCompleted = true;
            setResult(ActivityResult(std::move(result)));
            finish();
            return;
          }
        }
        requestUpdate(true);
      };
      startActivityForResult(std::make_unique<Ao3IndexActivity>(renderer, mappedInput, Ao3IndexMode::SINGLE, filePath),
                             handler);
    } else if (selectorIndex == 2) {
      if (AO3_MARKED_FOR_LATER_STORE.contains(filePath)) {
        AO3_MARKED_FOR_LATER_STORE.removeByPath(filePath);
        markedForLaterChanged = true;
      } else {
        Epub epub(filePath, "/.crosspoint");
        epub.load(false, true, Epub::XLocationLoadMode::Skip);
        if (AO3_MARKED_FOR_LATER_STORE.addBook(filePath, epub.getTitle(), epub.getAuthor())) {
          markedForLaterChanged = true;
        } else {
          RenderLock lock(*this);
          BookActions::drawToast(renderer, tr(STR_MARKED_FOR_LATER_LIMIT_REACHED));
        }
      }
      requestUpdate(true);
    } else if (selectorIndex == 3) {
      if (bookIsArchived) {
        const std::string restoredPath = Ao3ArchiveUtils::restoreFic(filePath);
        if (!restoredPath.empty()) {
          filePath = restoredPath;
          bookIsArchived = false;
          wasRestored = true;
        }
        requestUpdate(true);
      } else {
        // Archiving moves the file out of the tracked AO3 folder -- less
        // casually reversible than Restore, so confirm it the same way
        // Delete does below rather than acting immediately.
        auto handler = [this](const ActivityResult& res) {
          if (!res.isCancelled) {
            Epub epub(filePath, "/.crosspoint");
            epub.load(false, true, Epub::XLocationLoadMode::Skip);
            const std::string archivedPath =
                Ao3ArchiveUtils::archiveFic(filePath, epub.getTitle(), epub.getAuthor());
            if (!archivedPath.empty()) {
              // The book is no longer at the browsed path -- leave this menu
              // rather than keep operating on a stale filePath.
              BookActionResult result;
              result.modified = true;
              result.archived = true;
              setResult(ActivityResult(std::move(result)));
              finish();
              return;
            }
          }
          requestUpdate(true);
        };
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_ARCHIVE_CONFIRM_HEADING),
                                                    tr(STR_ARCHIVE_CONFIRM_BODY)),
            handler);
      }
    } else {
      // Trigger delete confirmation
      auto handler = [this](const ActivityResult& res) {
        if (!res.isCancelled) {
          BookActionResult result;
          result.deleted = true;
          result.modified = true;
          setResult(ActivityResult(std::move(result)));
          finish();
        } else {
          requestUpdate(true);
        }
      };
      std::string heading = std::string(tr(STR_DELETE)) + "?";
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, fileName), handler);
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectorIndex = (selectorIndex < ROW_COUNT - 1) ? selectorIndex + 1 : 0;
    requestUpdate(true);
  });

  buttonNavigator.onPrevious([this] {
    selectorIndex = (selectorIndex > 0) ? selectorIndex - 1 : ROW_COUNT - 1;
    requestUpdate(true);
  });
}

void BookActionActivity::saveStatus() {
  std::string cachePath = Epub::cachePathForFilePath(filePath, "/.crosspoint");
  Ao3Librarian::saveBookStatus(cachePath, currentStatus);

  // Sync finished flag to AO3 index (only on boundary crossing)
  if (hasAo3LibraryInfo) {
    bool isNowFinished = (currentStatus == BookStatus::FINISHED);
    bool wasFinished = (initialStatus == BookStatus::FINISHED);
    if (isNowFinished != wasFinished) {
      Ao3Librarian::setRecordFinished(filePath, isNowFinished);
    }
  }
}
