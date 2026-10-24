#include <gtest/gtest.h>
#include <Serialization.h>
#include <uzlib.h>

#include <string>

#include "BookmarkStore.h"

// Round-trip coverage for the bookFusionId field added to BookmarkStore's on-disk format for the
// BookFusion bookmark push (see global ~/.claude/CLAUDE.md's "InsiderPhD/crosspoint-reader port
// review" note): until now this field was compile-verified only, with no test proving it actually
// survives a save/reload cycle or that a pre-existing (pre-bookFusionId) file migrates safely.
namespace {

constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";

// Mirrors BookmarkStore.cpp's own (anonymous-namespace, non-exported) currentStoreFilePathForBook
// so this test can place/locate a file the real store will recognize without needing a private
// hook into the class under test.
std::string storeFilePathFor(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

class BookmarkStorePersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    BOOKMARKS.unload();
  }
  void TearDown() override {
    BOOKMARKS.unload();
    Storage.reset();
  }
};

}  // namespace

TEST_F(BookmarkStorePersistenceTest, BookFusionIdRoundTripsAcrossReload) {
  const std::string path = "/book.epub";
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.addBookmark(2, 0.5f, 100, "Chapter 2"), BookmarkStore::AddResult::Added);
  ASSERT_TRUE(BOOKMARKS.setBookmarkFusionId(0, 777u));

  // Simulate the book being closed and reopened -- the real-world case the push feature actually
  // depends on: a bookFusionId set on one visit must still be there in a later session, so a
  // repush doesn't create a duplicate bookmark server-side.
  BOOKMARKS.unload();

  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 1u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].bookFusionId, 777u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].spineIndex, 2);
}

TEST_F(BookmarkStorePersistenceTest, PreBookFusionIdFileMigratesIdToZeroNotGarbage) {
  const std::string path = "/legacy.epub";
  constexpr uint8_t SNIPPET_VERSION = 5;  // the format immediately before bookFusionId was added

  FsFile f;
  ASSERT_TRUE(Storage.openFileForWrite("BKS", storeFilePathFor(path, "epub"), f));
  serialization::writePod(f, SNIPPET_VERSION);
  serialization::writePod<uint16_t>(f, 1);  // count
  serialization::writeString(f, std::string("Title"));
  serialization::writeString(f, std::string("Author"));
  serialization::writeString(f, path);
  // One record in the SNIPPET_VERSION layout: spineIndex, progress, timestamp, chapterTitle[48],
  // paragraphIndex, snippet[64] -- stops there; no bookFusionId field exists at this version.
  serialization::writePod<uint16_t>(f, 3);   // spineIndex
  serialization::writePod<float>(f, 0.25f);  // progress
  serialization::writePod<uint32_t>(f, 0);   // timestamp
  char chapterTitle[BOOKMARK_CHAPTER_TITLE_MAX] = "Legacy Chapter";
  f.write(reinterpret_cast<const uint8_t*>(chapterTitle), sizeof(chapterTitle));
  serialization::writePod<uint16_t>(f, 7);  // paragraphIndex
  char snippet[BOOKMARK_SNIPPET_MAX] = "legacy snippet";
  f.write(reinterpret_cast<const uint8_t*>(snippet), sizeof(snippet));
  f.close();

  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 1u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].bookFusionId, 0u) << "pre-bookFusionId file must default to 0, not garbage";
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].spineIndex, 3);
  EXPECT_STREQ(BOOKMARKS.getBookmarks()[0].chapterTitle, "Legacy Chapter");

  // Loading an old-version file auto-migrates and rewrites it in the current format
  // (BookmarkStore::readFromFile()). A newly-set id on that migrated record must then round-trip
  // like any other -- the append-only-field discipline should need no special case for "this
  // record predates the field."
  ASSERT_TRUE(BOOKMARKS.setBookmarkFusionId(0, 999u));
  BOOKMARKS.unload();
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 1u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].bookFusionId, 999u);
}

TEST_F(BookmarkStorePersistenceTest, MultipleBookmarksKeepIndependentBookFusionIds) {
  const std::string path = "/multi.epub";
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.addBookmark(0, 0.1f, 100, "Ch1"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(BOOKMARKS.addBookmark(1, 0.4f, 100, "Ch2"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(BOOKMARKS.addBookmark(2, 0.9f, 100, "Ch3"), BookmarkStore::AddResult::Added);
  // Only the middle one has been pushed to BookFusion so far -- the others stay at the "never
  // pushed" sentinel (0), which must not collide with a real low-valued server id.
  ASSERT_TRUE(BOOKMARKS.setBookmarkFusionId(1, 42u));

  BOOKMARKS.unload();
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 3u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].bookFusionId, 0u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[1].bookFusionId, 42u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[2].bookFusionId, 0u);
}
