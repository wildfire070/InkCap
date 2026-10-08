#pragma once

// Pure guard predicates factored out of EpubReaderActivity so they're directly
// unit-testable without a live reader/Epub instance -- the same "extract the
// untestable logic out of the Activity file" pattern already used for
// ReaderSettingsSnapshotIO and DictionaryWordMeasure.
//
// Each mirrors a boolean condition that gates whether a book reaches the
// plain BookMoveUtils path. AO3 fics must always answer false here: they
// archive/restore exclusively through Ao3ArchiveUtils (see the reader's
// CYCLE_STATUS handler), and letting one reach the plain path leaves the AO3
// index pointing at a stale location. See EpubReaderActivity.cpp's call
// sites for full context on each.
namespace ReaderAo3MoveGuards {

// handleBookStatsReturn()'s arm, the atEndOfBook arm in loop(), and
// setBookCompleted()'s arm all answer this identical question before
// arming pendingReadFolderMove for the upstream moveFinishedToReadFolder
// feature.
inline bool shouldArmReadFolderMove(bool moveFinishedToReadFolderSetting, bool hasAo3Info,
                                    bool alreadyInReadFolder) {
  return moveFinishedToReadFolderSetting && !hasAo3Info && !alreadyInReadFolder;
}

// requestArchiveMove()'s condition for queuing the plain "Archive this book?" prompt.
inline bool shouldQueueArchiveMovePrompt(bool moveFinishedToArchiveFolderSetting, bool hasEpub, bool hasAo3Info,
                                         bool alreadyInArchiveFolder) {
  return moveFinishedToArchiveFolderSetting && hasEpub && !hasAo3Info && !alreadyInArchiveFolder;
}

// requestArchiveRestore()'s symmetric counterpart.
inline bool shouldQueueArchiveRestorePrompt(bool moveFinishedToArchiveFolderSetting, bool hasEpub, bool hasAo3Info,
                                            bool isInArchiveFolder) {
  return moveFinishedToArchiveFolderSetting && hasEpub && !hasAo3Info && isInArchiveFolder;
}

// shouldQueueCompletionPromptOnChapterExit()'s early-exit chain -- true means
// "stop here, don't queue the plain Mark-as-Finished prompt", matching the
// sense of that function's own leading `if`.
inline bool completionPromptShouldSkip(bool completionPromptShown, bool completionPromptQueued, bool isCompleted,
                                       bool activeFootnotePreview, bool hasPendingFootnotePreviewAnchor,
                                       bool completionTriggerCrossed, bool hasEpub, bool hasAo3Info, bool hasSection,
                                       int sectionPageCount, int completionTriggerSpineIndex, bool sectionIsBuilding,
                                       bool sectionIsPartial) {
  return completionPromptShown || completionPromptQueued || isCompleted || activeFootnotePreview ||
         hasPendingFootnotePreviewAnchor || !completionTriggerCrossed || !hasEpub || hasAo3Info || !hasSection ||
         sectionPageCount == 0 || completionTriggerSpineIndex < 0 || sectionIsBuilding || sectionIsPartial;
}

// LONG_MENU_MARK_FINISHED's routing choice: Cycle Status instead of plain
// setBookCompleted() for an AO3 fic.
inline bool shouldRouteMarkFinishedToCycleStatus(bool hasEpub, bool hasAo3Info) { return hasEpub && hasAo3Info; }

}  // namespace ReaderAo3MoveGuards
