#include <gtest/gtest.h>

#include "Arduino.h"
#include "Ao3ReceiveUtils.h"
#include "HalStorage.h"

namespace {
class Ao3ReceiveUtilsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    fakeMillis = 0;
  }
};
}  // namespace

// -- titleAuthorFileName -----------------------------------------------------

TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameJoinsTitleAndAuthor) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("Coffee Shop AU", "Jane Doe"), "Coffee Shop AU - Jane Doe.epub");
}

TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameWithNoAuthorIsJustTheTitle) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("Solo Fic", ""), "Solo Fic.epub");
}

TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameWithEmptyTitleIsEmpty) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("", "Jane Doe"), "");
}

// AO3's own EPUBs list every author in one dc:creator joined by ", ".
TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameTakesOnlyFirstAo3StyleAuthor) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("Ensemble Fic", "Alice, Bob, Carol"), "Ensemble Fic - Alice.epub");
}

// FanFicFare uses " & " instead.
TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameTakesOnlyFirstFanFicFareStyleAuthor) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("Ensemble Fic", "Alice & Bob"), "Ensemble Fic - Alice.epub");
}

// Whichever separator appears earliest in the string wins, in case a display name itself
// happens to contain the other separator's literal text.
TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameUsesWhicheverSeparatorComesFirst) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("Fic", "Alice & Bob, Carol"), "Fic - Alice.epub");
}

TEST_F(Ao3ReceiveUtilsTest, TitleAuthorFileNameSanitizesIllegalCharacters) {
  EXPECT_EQ(Ao3ReceiveUtils::titleAuthorFileName("A/B: Title", "Au*thor"), "A_B_ Title - Au_thor.epub");
}

// -- uniqueFilePath -----------------------------------------------------------

TEST_F(Ao3ReceiveUtilsTest, UniqueFilePathReturnsPlainPathWhenNothingExists) {
  EXPECT_EQ(Ao3ReceiveUtils::uniqueFilePath("/AO3 Downloads", "Fic.epub"), "/AO3 Downloads/Fic.epub");
}

TEST_F(Ao3ReceiveUtilsTest, UniqueFilePathHandlesRootFolderWithoutDoubleSlash) {
  EXPECT_EQ(Ao3ReceiveUtils::uniqueFilePath("/", "Fic.epub"), "/Fic.epub");
}

TEST_F(Ao3ReceiveUtilsTest, UniqueFilePathAddsNumericSuffixOnCollision) {
  Storage.put("/AO3 Downloads/Fic.epub", "x");
  EXPECT_EQ(Ao3ReceiveUtils::uniqueFilePath("/AO3 Downloads", "Fic.epub"), "/AO3 Downloads/Fic (2).epub");
}

TEST_F(Ao3ReceiveUtilsTest, UniqueFilePathSkipsSuffixesAlreadyTaken) {
  Storage.put("/AO3 Downloads/Fic.epub", "x");
  Storage.put("/AO3 Downloads/Fic (2).epub", "x");
  Storage.put("/AO3 Downloads/Fic (3).epub", "x");
  EXPECT_EQ(Ao3ReceiveUtils::uniqueFilePath("/AO3 Downloads", "Fic.epub"), "/AO3 Downloads/Fic (4).epub");
}

// The exact bug class this fix targets: exhausting every numeric suffix up to
// MAX_UNIQUE_SUFFIX (99) must fall back to a still-unique name instead of returning "".
TEST_F(Ao3ReceiveUtilsTest, UniqueFilePathFallsBackWhenEveryNumericSuffixIsTaken) {
  Storage.put("/AO3 Downloads/Fic.epub", "x");
  for (int n = 2; n <= 99; n++) {
    Storage.put("/AO3 Downloads/Fic (" + std::to_string(n) + ").epub", "x");
  }
  fakeMillis = 123456;

  const std::string result = Ao3ReceiveUtils::uniqueFilePath("/AO3 Downloads", "Fic.epub");

  EXPECT_NE(result, "");
  EXPECT_EQ(result, "/AO3 Downloads/Fic (123456-0).epub");
}

// Even if millis() itself is frozen (as it can appear within a single tick), the fallback's
// own attempt counter still guarantees a name that isn't already taken.
TEST_F(Ao3ReceiveUtilsTest, UniqueFilePathFallbackStillTerminatesWhenMillisIsFrozen) {
  Storage.put("/AO3 Downloads/Fic.epub", "x");
  for (int n = 2; n <= 99; n++) {
    Storage.put("/AO3 Downloads/Fic (" + std::to_string(n) + ").epub", "x");
  }
  fakeMillis = 42;
  Storage.put("/AO3 Downloads/Fic (42-0).epub", "x");  // pre-occupy the first fallback attempt

  const std::string result = Ao3ReceiveUtils::uniqueFilePath("/AO3 Downloads", "Fic.epub");

  EXPECT_EQ(result, "/AO3 Downloads/Fic (42-1).epub");
}

// -- receiveFolder -------------------------------------------------------------

TEST_F(Ao3ReceiveUtilsTest, ReceiveFolderDefaultsWhenNoSettingsFile) {
  EXPECT_EQ(Ao3ReceiveUtils::receiveFolder(), "/AO3 Downloads");
}

TEST_F(Ao3ReceiveUtilsTest, ReceiveFolderDefaultsOnMalformedJson) {
  Storage.put("/.crosspoint/ao3_settings.json", "{not json");
  EXPECT_EQ(Ao3ReceiveUtils::receiveFolder(), "/AO3 Downloads");
}

TEST_F(Ao3ReceiveUtilsTest, ReceiveFolderDefaultsWhenKeyIsEmpty) {
  Storage.put("/.crosspoint/ao3_settings.json", R"({"receiveFolder": ""})");
  EXPECT_EQ(Ao3ReceiveUtils::receiveFolder(), "/AO3 Downloads");
}

TEST_F(Ao3ReceiveUtilsTest, ReceiveFolderUsesConfiguredValue) {
  Storage.put("/.crosspoint/ao3_settings.json", R"({"receiveFolder": "/Books/AO3"})");
  EXPECT_EQ(Ao3ReceiveUtils::receiveFolder(), "/Books/AO3");
}

TEST_F(Ao3ReceiveUtilsTest, ReceiveFolderTrimsTrailingSlash) {
  Storage.put("/.crosspoint/ao3_settings.json", R"({"receiveFolder": "/Books/AO3/"})");
  EXPECT_EQ(Ao3ReceiveUtils::receiveFolder(), "/Books/AO3");
}

// -- pending list ---------------------------------------------------------------

TEST_F(Ao3ReceiveUtilsTest, PendingListStartsEmpty) {
  EXPECT_FALSE(Ao3ReceiveUtils::hasPending());
  EXPECT_TRUE(Ao3ReceiveUtils::readPending().empty());
}

TEST_F(Ao3ReceiveUtilsTest, AppendPendingIsReadableAndDedupes) {
  Ao3ReceiveUtils::appendPending("/AO3 Downloads/A.epub");
  Ao3ReceiveUtils::appendPending("/AO3 Downloads/B.epub");
  Ao3ReceiveUtils::appendPending("/AO3 Downloads/A.epub");  // duplicate, should not double up

  const auto pending = Ao3ReceiveUtils::readPending();
  EXPECT_TRUE(Ao3ReceiveUtils::hasPending());
  ASSERT_EQ(pending.size(), 2u);
  EXPECT_EQ(pending[0], "/AO3 Downloads/A.epub");
  EXPECT_EQ(pending[1], "/AO3 Downloads/B.epub");
}

TEST_F(Ao3ReceiveUtilsTest, RemovePendingDropsOnlyThatEntry) {
  Ao3ReceiveUtils::appendPending("/AO3 Downloads/A.epub");
  Ao3ReceiveUtils::appendPending("/AO3 Downloads/B.epub");

  Ao3ReceiveUtils::removePending("/AO3 Downloads/A.epub");

  const auto pending = Ao3ReceiveUtils::readPending();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0], "/AO3 Downloads/B.epub");
}

TEST_F(Ao3ReceiveUtilsTest, RemovingLastPendingEntryClearsHasPending) {
  Ao3ReceiveUtils::appendPending("/AO3 Downloads/A.epub");
  Ao3ReceiveUtils::removePending("/AO3 Downloads/A.epub");
  EXPECT_FALSE(Ao3ReceiveUtils::hasPending());
}
