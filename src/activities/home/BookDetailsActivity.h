#pragma once

#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// Full-page "Book Info" panel: cover on the left, a label/value metadata table on
// the right (series, bookshelf, content rating, completion status, chapters,
// updated date, progress), and the story description (dc:description in the
// EPUB's OPF, read on demand via Epub::getDescription() -- not cached inline
// with the other fields since it's comparatively large and only this screen
// needs it) below, scrollable a page at a time when it overflows the screen.
// Ported from InsiderPhD's crosspoint-reader BookDetailsActivity, trimmed to
// the metadata InkCap's own Calibre-column EPUB parsing actually exposes -- no
// publisher/tags/star-rating fields. Sibling prev/next book navigation
// (Left/Right, matching InsiderPhD) is supported, but this Activity owns none
// of the list logic for it -- the caller passes whether a previous/next book
// exists, and Left/Right just finish() with a BookDetailsNavResult so the
// caller (which already has the real book list -- see each caller's own
// openBookDetails()-style helper) can re-launch this Activity for that book.
class BookDetailsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::string bookPath;
  std::string title;
  std::string author;
  std::string seriesName;
  std::string seriesIndex;
  std::string bookshelf;
  std::string contentRating;
  std::string completionStatus;
  std::string chapters;
  std::string updatedDate;
  std::string description;
  float progressPercent = -1.0f;
  bool hasPreviousBook = false;
  bool hasNextBook = false;

  std::string coverPath;
  int coverWidthPx = 0;           // 0 = use the 3:4 fallback (no cover to size against yet)
  int descScrollOffset = 0;      // first visible description line
  int descVisibleLines = 1;      // updated each render; used for page-step scrolling
  bool loading = false;
  bool waitForConfirmRelease = false;
  bool waitForBackRelease = false;

  void loadMetadata();

 public:
  explicit BookDetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                               std::string title, std::string author, bool hasPreviousBook = false,
                               bool hasNextBook = false)
      : Activity("BookDetails", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        title(std::move(title)),
        author(std::move(author)),
        hasPreviousBook(hasPreviousBook),
        hasNextBook(hasNextBook) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
