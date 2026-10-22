#pragma once

// Ao3Librarian.cpp's statusLabel() switches on these -- values just need to be distinct.
inline constexpr int STR_STATUS_UNREAD = 0;
inline constexpr int STR_STATUS_READING = 1;
inline constexpr int STR_STATUS_FINISHED = 2;
inline constexpr int STR_STATUS_WAITING_FOR_CHAPTER = 3;
inline constexpr int STR_STATUS_NEW_CHAPTER_AVAILABLE = 4;

inline const char* tr(int) { return ""; }
