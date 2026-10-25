#include <gtest/gtest.h>
#include <Serialization.h>
#include <uzlib.h>

#include <string>

#include "BookmarkStore.h"

// Round-trip coverage for BookmarkStore's on-disk format. Originally written to cover a
// per-bookmark bookFusionId field added for the BookFusion bookmark push -- that push turned out
// to hit a wrong, non-existent BookFusion endpoint (every create attempt failed server-side with
// "Device required!"; BookFusion's real API has no concept of a plain, text-less bookmark at
// all, only highlighted passages with real quoted text -- see
// BookFusionSyncActivity::pushHighlights()'s comment) and the field was removed along with it.
// Kept and retargeted at the store's other fields plus the version-6-with-bookFusionId file
// format that briefly shipped, which BookmarkStore::readFromFile() still reads (skipping the
// stray 4 bytes) so a device that already wrote one doesn't hit "unknown version".
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

TEST_F(BookmarkStorePersistenceTest, BasicFieldsRoundTripAcrossReload) {
  const std::string path = "/book.epub";
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.addBookmark(2, 0.5f, 100, "Chapter 2", 7, "a short snippet"), BookmarkStore::AddResult::Added);

  // Simulate the book being closed and reopened -- every field set on one visit must still be
  // there in a later session.
  BOOKMARKS.unload();

  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 1u);
  const Bookmark& bm = BOOKMARKS.getBookmarks()[0];
  EXPECT_EQ(bm.spineIndex, 2);
  EXPECT_FLOAT_EQ(bm.progress, 0.5f);
  EXPECT_STREQ(bm.chapterTitle, "Chapter 2");
  EXPECT_EQ(bm.paragraphIndex, 7);
  EXPECT_STREQ(bm.snippet, "a short snippet");
}

TEST_F(BookmarkStorePersistenceTest, LegacyBookFusionIdFileLoadsAndMigrates) {
  const std::string path = "/legacy.epub";
  // The format briefly shipped with a per-bookmark bookFusionId trailing each record (now
  // removed -- see this file's header comment). BookmarkStore.cpp's own
  // LEGACY_BOOKFUSION_ID_VERSION (internal constant); duplicated here since it's not exported.
  constexpr uint8_t LEGACY_BOOKFUSION_ID_VERSION = 6;

  FsFile f;
  ASSERT_TRUE(Storage.openFileForWrite("BKS", storeFilePathFor(path, "epub"), f));
  serialization::writePod(f, LEGACY_BOOKFUSION_ID_VERSION);
  serialization::writePod<uint16_t>(f, 1);  // count
  serialization::writeString(f, std::string("Title"));
  serialization::writeString(f, std::string("Author"));
  serialization::writeString(f, path);
  // One record in the LEGACY_BOOKFUSION_ID_VERSION layout: spineIndex, progress, timestamp,
  // chapterTitle[48], paragraphIndex, snippet[64], bookFusionId(uint32) -- the trailing 4 bytes
  // this version's reader must skip rather than error on.
  serialization::writePod<uint16_t>(f, 3);   // spineIndex
  serialization::writePod<float>(f, 0.25f);  // progress
  serialization::writePod<uint32_t>(f, 0);   // timestamp
  char chapterTitle[BOOKMARK_CHAPTER_TITLE_MAX] = "Legacy Chapter";
  f.write(reinterpret_cast<const uint8_t*>(chapterTitle), sizeof(chapterTitle));
  serialization::writePod<uint16_t>(f, 7);  // paragraphIndex
  char snippet[BOOKMARK_SNIPPET_MAX] = "legacy snippet";
  f.write(reinterpret_cast<const uint8_t*>(snippet), sizeof(snippet));
  serialization::writePod<uint32_t>(f, 555);  // bookFusionId -- must be skipped, not kept anywhere
  f.close();

  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 1u);
  const Bookmark& bm = BOOKMARKS.getBookmarks()[0];
  EXPECT_EQ(bm.spineIndex, 3);
  EXPECT_STREQ(bm.chapterTitle, "Legacy Chapter");
  EXPECT_EQ(bm.paragraphIndex, 7);
  EXPECT_STREQ(bm.snippet, "legacy snippet");

  // Loading an old-version file auto-migrates and rewrites it in the current format
  // (BookmarkStore::readFromFile()'s needsRewrite). Add a second bookmark and reload again to
  // confirm the migrated file is still readable in the (now current, bookFusionId-less) format.
  ASSERT_EQ(BOOKMARKS.addBookmark(5, 0.75f, 100, "Chapter 5"), BookmarkStore::AddResult::Added);
  BOOKMARKS.unload();
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 2u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].spineIndex, 3);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[1].spineIndex, 5);
}

TEST_F(BookmarkStorePersistenceTest, MultipleBookmarksKeepIndependentFields) {
  const std::string path = "/multi.epub";
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.addBookmark(0, 0.1f, 100, "Ch1"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(BOOKMARKS.addBookmark(1, 0.4f, 100, "Ch2"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(BOOKMARKS.addBookmark(2, 0.9f, 100, "Ch3"), BookmarkStore::AddResult::Added);

  BOOKMARKS.unload();
  ASSERT_TRUE(BOOKMARKS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(BOOKMARKS.getBookmarks().size(), 3u);
  EXPECT_EQ(BOOKMARKS.getBookmarks()[0].spineIndex, 0);
  EXPECT_STREQ(BOOKMARKS.getBookmarks()[0].chapterTitle, "Ch1");
  EXPECT_EQ(BOOKMARKS.getBookmarks()[1].spineIndex, 1);
  EXPECT_STREQ(BOOKMARKS.getBookmarks()[1].chapterTitle, "Ch2");
  EXPECT_EQ(BOOKMARKS.getBookmarks()[2].spineIndex, 2);
  EXPECT_STREQ(BOOKMARKS.getBookmarks()[2].chapterTitle, "Ch3");
}
