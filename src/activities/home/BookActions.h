#pragma once

#include <memory>
#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"
#include "PendingOverlayResume.h"

class GfxRenderer;
class MappedInputManager;
class Activity;

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
// EPUB: reboot into progress sync for this book. XTC: send its stats in place.
void syncProgress(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& fullPath,
                  PendingOverlayResume returnResume);
bool hasClearableBookCache(const std::string& path);
bool canSendNearby(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
std::unique_ptr<Activity> createReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::string& fullPath, const std::string& title);
bool resetBookReaderSettings(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
// True when marking `fullPath` finished right now would also move it into /Archive (the move setting is on, the
// book is an epub that is not finished yet and is not already there). Callers ask before doing that.
bool completingWouldArchive(const std::string& fullPath);
// Symmetric counterpart: true when marking `fullPath` unfinished right now would also move it back out of
// /Archive (the move setting is on, the book is an epub that is finished and currently archived). Callers ask
// before doing that, the same way they ask before completingWouldArchive's move.
bool uncompletingWouldRestore(const std::string& fullPath);
// Read-only, log-quiet variant for list rows: never migrates cache folders or
// stats files, and skips books that have no cache folder.
bool isBookCompletedForList(const std::string& fullPath);
bool isBookStatsTrackingEnabled(const std::string& fullPath);
bool toggleBookStatsTracking(const std::string& fullPath, bool& enabled);
// Sets a book's Finished status directly (not a toggle) given only its path, with no archive-move side effect --
// the shared "set finished" write (stats + global count + recents) used by every place that flips Finished as a
// side effect of another action (Archive/Restore), where the caller already handled any move itself. `fullPath`
// must be the book's CURRENT path (e.g. the path archiveBook()/restoreBook() just returned, not its old one).
bool setBookCompletedOnDisk(const std::string& fullPath, bool completed);
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed,
                         bool allowMove = true);
void drawToast(const GfxRenderer& renderer, const char* msg);

}  // namespace BookActions
