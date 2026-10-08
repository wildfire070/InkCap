#include "ReaderAo3MoveGuards.h"

#include <gtest/gtest.h>

using namespace ReaderAo3MoveGuards;

// --- shouldArmReadFolderMove ---
// Regression coverage for gap 1/the atEndOfBook arm/setBookCompleted's arm
// (fixed d837de42/e31a8b23/906b11e3): the upstream moveFinishedToReadFolder
// feature must never fire for an AO3 fic.

TEST(ShouldArmReadFolderMove, TrueWhenSettingOnNotAo3NotAlreadyThere) {
  EXPECT_TRUE(shouldArmReadFolderMove(/*settingOn=*/true, /*hasAo3Info=*/false, /*alreadyInReadFolder=*/false));
}

TEST(ShouldArmReadFolderMove, FalseForAo3FicEvenWithSettingOnAndNotAlreadyThere) {
  EXPECT_FALSE(shouldArmReadFolderMove(/*settingOn=*/true, /*hasAo3Info=*/true, /*alreadyInReadFolder=*/false));
}

TEST(ShouldArmReadFolderMove, FalseWhenSettingOff) {
  EXPECT_FALSE(shouldArmReadFolderMove(/*settingOn=*/false, /*hasAo3Info=*/false, /*alreadyInReadFolder=*/false));
}

TEST(ShouldArmReadFolderMove, FalseWhenAlreadyInReadFolder) {
  EXPECT_FALSE(shouldArmReadFolderMove(/*settingOn=*/true, /*hasAo3Info=*/false, /*alreadyInReadFolder=*/true));
}

// --- shouldQueueArchiveMovePrompt ---
// Regression coverage for requestArchiveMove() (gap 4, fixed via
// c78c4ed8/dec422c1/9523d077's hasAo3Info guard, deepened by the later
// hasEpub parameter).

TEST(ShouldQueueArchiveMovePrompt, TrueWhenSettingOnHasEpubNotAo3NotAlreadyArchived) {
  EXPECT_TRUE(shouldQueueArchiveMovePrompt(/*settingOn=*/true, /*hasEpub=*/true, /*hasAo3Info=*/false,
                                           /*alreadyInArchiveFolder=*/false));
}

TEST(ShouldQueueArchiveMovePrompt, FalseForAo3FicEvenWithEverythingElseTrue) {
  EXPECT_FALSE(shouldQueueArchiveMovePrompt(/*settingOn=*/true, /*hasEpub=*/true, /*hasAo3Info=*/true,
                                            /*alreadyInArchiveFolder=*/false));
}

TEST(ShouldQueueArchiveMovePrompt, FalseWithoutEpub) {
  EXPECT_FALSE(shouldQueueArchiveMovePrompt(/*settingOn=*/true, /*hasEpub=*/false, /*hasAo3Info=*/false,
                                            /*alreadyInArchiveFolder=*/false));
}

TEST(ShouldQueueArchiveMovePrompt, FalseWhenAlreadyInArchiveFolder) {
  EXPECT_FALSE(shouldQueueArchiveMovePrompt(/*settingOn=*/true, /*hasEpub=*/true, /*hasAo3Info=*/false,
                                            /*alreadyInArchiveFolder=*/true));
}

// --- shouldQueueArchiveRestorePrompt ---
// Regression coverage for requestArchiveRestore() (gap 5).

TEST(ShouldQueueArchiveRestorePrompt, TrueWhenSettingOnHasEpubNotAo3AndInArchiveFolder) {
  EXPECT_TRUE(shouldQueueArchiveRestorePrompt(/*settingOn=*/true, /*hasEpub=*/true, /*hasAo3Info=*/false,
                                              /*isInArchiveFolder=*/true));
}

TEST(ShouldQueueArchiveRestorePrompt, FalseForAo3FicEvenWithEverythingElseTrue) {
  EXPECT_FALSE(shouldQueueArchiveRestorePrompt(/*settingOn=*/true, /*hasEpub=*/true, /*hasAo3Info=*/true,
                                               /*isInArchiveFolder=*/true));
}

TEST(ShouldQueueArchiveRestorePrompt, FalseWhenNotInArchiveFolder) {
  EXPECT_FALSE(shouldQueueArchiveRestorePrompt(/*settingOn=*/true, /*hasEpub=*/true, /*hasAo3Info=*/false,
                                               /*isInArchiveFolder=*/false));
}

// --- completionPromptShouldSkip ---
// Regression coverage for shouldQueueCompletionPromptOnChapterExit() (gap 2):
// an AO3 fic reaching the last page of its completion-trigger chapter must
// never queue the plain "Mark as Finished?" prompt, regardless of every
// other condition being otherwise satisfied.

namespace {
// All-false/zero baseline where nothing blocks the prompt from queuing.
struct Baseline {
  bool completionPromptShown = false;
  bool completionPromptQueued = false;
  bool isCompleted = false;
  bool activeFootnotePreview = false;
  bool hasPendingFootnotePreviewAnchor = false;
  bool completionTriggerCrossed = true;
  bool hasEpub = true;
  bool hasAo3Info = false;
  bool hasSection = true;
  int sectionPageCount = 10;
  int completionTriggerSpineIndex = 0;
  bool sectionIsBuilding = false;
  bool sectionIsPartial = false;

  bool skip() const {
    return completionPromptShouldSkip(completionPromptShown, completionPromptQueued, isCompleted,
                                      activeFootnotePreview, hasPendingFootnotePreviewAnchor,
                                      completionTriggerCrossed, hasEpub, hasAo3Info, hasSection, sectionPageCount,
                                      completionTriggerSpineIndex, sectionIsBuilding, sectionIsPartial);
  }
};
}  // namespace

TEST(CompletionPromptShouldSkip, FalseAtBaseline) { EXPECT_FALSE(Baseline{}.skip()); }

TEST(CompletionPromptShouldSkip, TrueForAo3FicEvenAtOtherwiseClearBaseline) {
  Baseline b;
  b.hasAo3Info = true;
  EXPECT_TRUE(b.skip());
}

TEST(CompletionPromptShouldSkip, TrueWhenAlreadyShownOrQueuedOrCompleted) {
  Baseline shown;
  shown.completionPromptShown = true;
  EXPECT_TRUE(shown.skip());

  Baseline queued;
  queued.completionPromptQueued = true;
  EXPECT_TRUE(queued.skip());

  Baseline completed;
  completed.isCompleted = true;
  EXPECT_TRUE(completed.skip());
}

TEST(CompletionPromptShouldSkip, TrueWithoutEpubOrSection) {
  Baseline noEpub;
  noEpub.hasEpub = false;
  EXPECT_TRUE(noEpub.skip());

  Baseline noSection;
  noSection.hasSection = false;
  EXPECT_TRUE(noSection.skip());
}

TEST(CompletionPromptShouldSkip, TrueWhileSectionBuildingOrPartialOrEmpty) {
  Baseline building;
  building.sectionIsBuilding = true;
  EXPECT_TRUE(building.skip());

  Baseline partial;
  partial.sectionIsPartial = true;
  EXPECT_TRUE(partial.skip());

  Baseline empty;
  empty.sectionPageCount = 0;
  EXPECT_TRUE(empty.skip());
}

// --- shouldRouteMarkFinishedToCycleStatus ---
// Regression coverage for the LONG_MENU_MARK_FINISHED quick-action shortcut
// (gap 3): it must route an AO3 fic to Cycle Status instead of calling
// setBookCompleted() directly.

TEST(ShouldRouteMarkFinishedToCycleStatus, TrueWhenHasEpubAndAo3Info) {
  EXPECT_TRUE(shouldRouteMarkFinishedToCycleStatus(/*hasEpub=*/true, /*hasAo3Info=*/true));
}

TEST(ShouldRouteMarkFinishedToCycleStatus, FalseWhenNotAo3) {
  EXPECT_FALSE(shouldRouteMarkFinishedToCycleStatus(/*hasEpub=*/true, /*hasAo3Info=*/false));
}

TEST(ShouldRouteMarkFinishedToCycleStatus, FalseWithoutEpub) {
  EXPECT_FALSE(shouldRouteMarkFinishedToCycleStatus(/*hasEpub=*/false, /*hasAo3Info=*/true));
}
