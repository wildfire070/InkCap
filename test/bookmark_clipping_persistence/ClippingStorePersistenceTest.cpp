#include <gtest/gtest.h>
#include <HalStorage.h>

#include <string>

#include "ClippingStore.h"

// Round-trip coverage for the bookFusionId field added to ClippingStore's on-disk format for the
// BookFusion highlight push (see global ~/.claude/CLAUDE.md's "InsiderPhD/crosspoint-reader port
// review" note) -- previously compile-verified only, same as BookmarkStore's equivalent field
// (see BookmarkStorePersistenceTest.cpp). Exercises the real public API (addClipping /
// setClippingFusionId / saveToFile / loadForBook), not the private read/write internals, so it
// proves what production code actually depends on.
namespace {

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
