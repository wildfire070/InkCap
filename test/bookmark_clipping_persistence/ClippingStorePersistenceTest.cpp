#include <gtest/gtest.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <uzlib.h>

#include <string>

#include "ClippingStore.h"

// Round-trip coverage for the bookFusionId field added to ClippingStore's on-disk format for the
// BookFusion highlight push (see global ~/.claude/CLAUDE.md's "InsiderPhD/crosspoint-reader port
// review" note) -- previously compile-verified only, same as BookmarkStore's equivalent field
// (see BookmarkStorePersistenceTest.cpp). Exercises the real public API (addClipping /
// setClippingFusionId / saveToFile / loadForBook), not the private read/write internals, so it
// proves what production code actually depends on.
namespace {

constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";

// Mirrors ClippingStore.cpp's own (anonymous-namespace, non-exported) storeFilePathForBook so this
// test can place/locate a file the real store will recognize without needing a private hook into
// the class under test.
std::string storeFilePathFor(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

class ClippingStorePersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    CLIPPINGS.unload();
  }
  void TearDown() override {
    CLIPPINGS.unload();
    Storage.reset();
  }
};

}  // namespace

TEST_F(ClippingStorePersistenceTest, BookFusionIdRoundTripsAcrossReload) {
  const std::string path = "/book.epub";
  ASSERT_TRUE(CLIPPINGS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(CLIPPINGS.addClipping(2, 10, 11, 1, 0, 5, 6, "Chapter 2", 3, "a short highlight", UINT16_MAX, 0),
            ClippingStore::AddResult::Added);
  ASSERT_TRUE(CLIPPINGS.setClippingFusionId(0, 555u));

  // Simulate the book being closed and reopened -- the real-world case the push feature actually
  // depends on: a bookFusionId set on one visit must still be there in a later session, so a
  // repush doesn't create a duplicate highlight server-side.
  CLIPPINGS.unload();

  ASSERT_TRUE(CLIPPINGS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1u);
  const Clipping* clipping = CLIPPINGS.clippingAt(0);
  ASSERT_NE(clipping, nullptr);
  EXPECT_EQ(clipping->bookFusionId, 555u);
  EXPECT_EQ(clipping->spineIndex, 2);
  EXPECT_STREQ(clipping->chapterTitle, "Chapter 2");

  std::string text;
  ASSERT_TRUE(CLIPPINGS.readClippingText(0, text));
  EXPECT_EQ(text, "a short highlight");
}

TEST_F(ClippingStorePersistenceTest, PreBookFusionIdFileMigratesIdToZeroNotGarbage) {
  const std::string path = "/legacy.epub";
  // TABLE_SELECTION_VERSION (ClippingStore.cpp's internal constant) -- the format immediately
  // before bookFusionId was added. Has layoutSignature and tableSelection but no bookFusionId.
  constexpr uint8_t TABLE_SELECTION_VERSION = 4;

  FsFile f;
  ASSERT_TRUE(Storage.openFileForWrite("CLIP", storeFilePathFor(path, "epub"), f));
  serialization::writePod(f, TABLE_SELECTION_VERSION);
  serialization::writePod<uint16_t>(f, 1);  // count
  serialization::writeString(f, std::string("Title"));
  serialization::writeString(f, std::string("Author"));
  serialization::writeString(f, path);
  // One record in the TABLE_SELECTION_VERSION layout: spineIndex, startPage, endPage, pageCount,
  // startWordIndex, endWordIndex, wordCount, paragraphIndex, timestamp, layoutSignature,
  // tableSelection, chapterTitle[48], textLength, text -- stops there; no bookFusionId field
  // exists at this version (readFromFile only reads it when version >= VERSION(5)).
  serialization::writePod<uint16_t>(f, 4);      // spineIndex
  serialization::writePod<uint16_t>(f, 10);     // startPage
  serialization::writePod<uint16_t>(f, 10);     // endPage
  serialization::writePod<uint16_t>(f, 20);     // pageCount
  serialization::writePod<uint16_t>(f, 1);      // startWordIndex
  serialization::writePod<uint16_t>(f, 2);      // endWordIndex
  serialization::writePod<uint16_t>(f, 2);      // wordCount
  serialization::writePod<uint16_t>(f, 6);      // paragraphIndex
  serialization::writePod<uint32_t>(f, 0);      // timestamp
  serialization::writePod<uint32_t>(f, 12345);  // layoutSignature
  serialization::writePod<uint16_t>(f, 0xFFFF); // tableSelection (UINT16_MAX = none)
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = "Legacy Chapter";
  f.write(reinterpret_cast<const uint8_t*>(chapterTitle), sizeof(chapterTitle));
  const std::string legacyText = "legacy clip";
  serialization::writePod<uint16_t>(f, static_cast<uint16_t>(legacyText.size()));
  f.write(reinterpret_cast<const uint8_t*>(legacyText.data()), legacyText.size());
  f.close();

  ASSERT_TRUE(CLIPPINGS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1u);
  const Clipping* clipping = CLIPPINGS.clippingAt(0);
  ASSERT_NE(clipping, nullptr);
  EXPECT_EQ(clipping->bookFusionId, 0u) << "pre-bookFusionId file must default to 0, not garbage";
  EXPECT_EQ(clipping->spineIndex, 4);
  EXPECT_STREQ(clipping->chapterTitle, "Legacy Chapter");
  std::string text;
  ASSERT_TRUE(CLIPPINGS.readClippingText(0, text));
  EXPECT_EQ(text, legacyText);

  // Loading an old-version file auto-migrates and rewrites it in the current format
  // (ClippingStore::writeToFile() always writes the current VERSION). A newly-set id on that
  // migrated record must then round-trip like any other -- the append-only-field discipline
  // should need no special case for "this record predates the field."
  ASSERT_TRUE(CLIPPINGS.setClippingFusionId(0, 888u));
  CLIPPINGS.unload();
  ASSERT_TRUE(CLIPPINGS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1u);
  EXPECT_EQ(CLIPPINGS.clippingAt(0)->bookFusionId, 888u);
}

TEST_F(ClippingStorePersistenceTest, MultipleClippingsKeepIndependentBookFusionIds) {
  const std::string path = "/multi.epub";
  ASSERT_TRUE(CLIPPINGS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(CLIPPINGS.addClipping(0, 1, 1, 1, 0, 2, 3, "Ch1", UINT16_MAX, "first", UINT16_MAX, 0),
            ClippingStore::AddResult::Added);
  ASSERT_EQ(CLIPPINGS.addClipping(1, 4, 4, 1, 0, 2, 3, "Ch2", UINT16_MAX, "second", UINT16_MAX, 0),
            ClippingStore::AddResult::Added);
  // Only the first has been pushed to BookFusion so far -- the other stays at the "never pushed"
  // sentinel (0), which must not collide with a real low-valued server id.
  ASSERT_TRUE(CLIPPINGS.setClippingFusionId(0, 11u));

  CLIPPINGS.unload();
  ASSERT_TRUE(CLIPPINGS.loadForBook(path, "Title", "Author", "epub"));
  ASSERT_EQ(CLIPPINGS.clippingCount(), 2u);
  EXPECT_EQ(CLIPPINGS.clippingAt(0)->bookFusionId, 11u);
  EXPECT_EQ(CLIPPINGS.clippingAt(1)->bookFusionId, 0u);
}
