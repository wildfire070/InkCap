#pragma once

using StrId = int;

constexpr StrId STR_BOOK = 1;
constexpr StrId STR_CHAPTER = 2;
constexpr StrId STR_PAGES_SEPARATOR = 3;

inline const char* tr(const StrId) { return "translated"; }
