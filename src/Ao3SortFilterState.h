#pragma once
#include <stdint.h>

enum class SortMode : uint8_t { ALPHABETIC = 0, WORD_COUNT = 1, DATE_ADDED = 2, SERIES = 3, AUTHOR = 4 };

// Which slice of the library the list shows. Persisted values -- append only.
//   MARKED_FOR_LATER / NEW_CHAPTERS list their stores' fics in the store's own order
//   (queue order / most-recently-updated first), ignoring the Sort By / Order rows.
//   WIPS is every indexed fic the author hasn't finished (isCompleted == 0).
enum class LibraryView : uint8_t { ALL = 0, MARKED_FOR_LATER = 1, NEW_CHAPTERS = 2, WIPS = 3 };

struct SortFilterState {
  // Folder Tree mode only — folder-name filters, unrelated to rating/completion below.
  char fandom[32] = {};
  char relationship[32] = {};
  bool relationshipNoneOnly = false;

  // Automatic mode only.
  char rating = 0;         // 0 = Any, else one of 'G','T','M','E','-'
  int8_t completion = -1;  // -1 = Any, 0 = Incomplete, 1 = Complete

  SortMode sortMode = SortMode::ALPHABETIC;
  bool ascending = true;

  LibraryView view = LibraryView::ALL;
};
