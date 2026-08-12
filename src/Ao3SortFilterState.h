#pragma once
#include <stdint.h>

enum class SortMode : uint8_t {
    ALPHABETIC  = 0,
    WORD_COUNT  = 1,
    DATE_ADDED  = 2,
    SERIES      = 3,
    AUTHOR      = 4
};

struct SortFilterState {
    // Folder Tree mode only — folder-name filters, unrelated to rating/completion below.
    char     fandom[32] = {};
    char     relationship[32] = {};
    bool     relationshipNoneOnly = false;

    // Automatic mode only.
    char     rating = 0;        // 0 = Any, else one of 'G','T','M','E','-'
    int8_t   completion = -1;   // -1 = Any, 0 = Incomplete, 1 = Complete

    SortMode sortMode = SortMode::ALPHABETIC;
    bool     ascending = true;
};
