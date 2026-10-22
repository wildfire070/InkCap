#pragma once

// Only psramHeapAvailable() is needed -- it's called inline from
// Ao3CompactIndexRecord.h's maxLibraryBooks(), pulled in transitively via Ao3Librarian.h, but
// nothing in Ao3Librarian.cpp calls maxLibraryBooks() itself in the paths this test exercises.
inline bool psramHeapAvailable() { return false; }
