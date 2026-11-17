// Regression coverage for the archived-fic guard at the top of Ao3Librarian::scrape()
// (see git history: "fix: guard archived fics from reindex..." followed by
// "fix: use cheap path check for scrape()'s archived-fic guard").
//
// Two real bugs lived here, in sequence:
//  1. (original, reported upstream by SiliconAves/AvesO3) scrape() had no guard at all, so
//     reindexing a fic sitting in /Archive/ wrote a live CompactIndexRecord for a path
//     Ao3ArchiveUtils::isArchived() expects to have none -- corrupting the archived/live
//     invariant restoreFic() relies on.
//  2. (introduced while fixing #1, caught on a later "confirm no bugs" pass) the first fix
//     used Ao3ArchiveUtils::isArchived() as the guard condition. isArchived() is "has a valid
//     sidecar but no live index record" -- which is exactly the transient state a fic is in
//     the moment Ao3ArchiveUtils::restoreFic() calls scrape(force=true) on it to CREATE that
//     live record after moving it out of /Archive/. The guard silently no-oped that very
//     call, so every successfully restored fic permanently vanished from the index with no
//     error anywhere.
//
// The fix settled on BookMoveUtils::isInArchiveFolder() -- a pure path-prefix check -- which
// cannot misfire on case 2 (the file has already moved out of /Archive/ by the time
// restoreFic() calls scrape() again) while still catching case 1 (a fic physically sitting in
// /Archive/). These tests pin that behavior directly against the real Ao3Librarian::scrape(),
// using a fake Epub whose getSpineItemsCount() is 0 by default -- which makes the real
// Ao3Librarian::parseTitlePage() return false immediately, without needing to fake real EPUB
// zip/HTML content. That makes "did scrape() take the archived-skip shortcut?" directly
// observable from its return value: true means the guard fired before parseTitlePage ever
// ran; false means it legitimately attempted real work and failed for lack of real content.
#include <gtest/gtest.h>

#include "Ao3CompactIndexRecord.h"
#include "Ao3Librarian.h"
#include "Ao3LibraryMetadata.h"
#include "Epub.h"
#include "HalStorage.h"
#include "ZipFile.h"

namespace {

constexpr const char* kIndexPath = "/.crosspoint/ao3_library_index.bin";

class Ao3LibrarianScrapeGuardTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
};

TEST_F(Ao3LibrarianScrapeGuardTest, ArchiveFolderPathSkipsWithoutWritingAnything) {
  Epub epub("/Archive/My Fic.epub", "/.crosspoint");

  const bool result = Ao3Librarian::scrape(epub, /*force=*/true);

  EXPECT_TRUE(result);
  // The guard must return before ever touching storage -- not just before writing the index,
  // but before even probing for an existing sidecar.
  EXPECT_FALSE(Storage.exists((epub.getCachePath() + "/ao3_library_info").c_str()));
  EXPECT_FALSE(Storage.exists(kIndexPath));
}

TEST_F(Ao3LibrarianScrapeGuardTest, NestedArchiveSubfolderPathAlsoSkips) {
  // archiveFic() always flattens to a direct child of /Archive/ in production, but the guard
  // itself is a prefix check, so a nested path should match it too.
  Epub epub("/Archive/sub/My Fic.epub", "/.crosspoint");

  EXPECT_TRUE(Ao3Librarian::scrape(epub, /*force=*/true));
  EXPECT_FALSE(Storage.exists(kIndexPath));
}

TEST_F(Ao3LibrarianScrapeGuardTest, SimilarButUnrelatedPathIsNotTreatedAsArchived) {
  // A path that merely starts with the same characters ("/ArchiveStuff/...") is NOT inside
  // /Archive/ and must not be caught by a sloppy substring/prefix mix-up.
  Epub epub("/ArchiveStuff/My Fic.epub", "/.crosspoint");
  epub.spineItemsCount = 0;  // forces parseTitlePage() to fail, see file comment

  EXPECT_FALSE(Ao3Librarian::scrape(epub, /*force=*/true));
}

TEST_F(Ao3LibrarianScrapeGuardTest, NonArchiveFolderPathIsNotSkipped) {
  // This is the restoreFic() moment: the fic has just been moved OUT of /Archive/ (so its path
  // is now an ordinary library path), and scrape(force=true) is being called specifically to
  // recreate its live index record. The old isArchived()-based guard would have returned true
  // here (silently skipping the write) because the sidecar/index state at that instant still
  // looks "archived" by that definition -- a valid sidecar (written below, matching what
  // restoreFic() finds via the migrated cache dir) with no live index record (there is none:
  // nothing has written to ao3_library_index.bin in this test at all). The path-based guard
  // must not be fooled by that -- it has to actually attempt the real work regardless of
  // sidecar/index state.
  Epub epub("/Books/My Fic.epub", "/.crosspoint");
  epub.spineItemsCount = 0;  // no real content to parse -- see file comment for why this is
                             // the right way to observe "did NOT take the archived shortcut"

  Ao3LibraryMetadata existingSidecar;  // default ctor sets a valid magic, matching isValid()
  fake::add(epub.getCachePath() + "/ao3_library_info",
            std::string(reinterpret_cast<const char*>(&existingSidecar), sizeof(existingSidecar)));

  const bool result = Ao3Librarian::scrape(epub, /*force=*/true);

  // false (not true) proves scrape() reached real parseTitlePage()-based logic rather than
  // early-returning true from the guard.
  EXPECT_FALSE(result);
}

// Coverage for the OPF-enrichment cascade added to scrape() (word count + pairing), separate
// from the archive-guard tests above. The fake title page below deliberately has only a work-ID
// link and no Words:/Relationship: label at all -- simulating FanFicFare's condensed template
// (confirmed via a real device-library survey to be the dominant case, not a rare one), where
// the HTML scrape alone would leave both fields empty. getWordCount()/getMainShip() simulate
// what the OPF custom columns would supply for such a file.
TEST_F(Ao3LibrarianScrapeGuardTest, OpfSuppliesWordCountAndMainShipTheCondensedTemplateCannot) {
  Epub epub("/Books/Hard Lines.epub", "/.crosspoint");
  epub.spineItemsCount = 1;
  epub.fakeTitlePageHtml = R"(<h1><a href="https://archiveofourown.org/works/59081659">Hard Lines</a></h1>)";
  epub.wordCountValue = "65450";
  epub.mainShipValue = "Hermione Granger/Draco Malfoy";

  ASSERT_TRUE(Ao3Librarian::scrape(epub, /*force=*/true));

  const auto sidecarIt = fake::files.find(epub.getCachePath() + "/ao3_library_info");
  ASSERT_NE(sidecarIt, fake::files.end());
  Ao3LibraryMetadata meta;
  ASSERT_EQ(sidecarIt->second->bytes.size(), sizeof(meta));
  std::memcpy(&meta, sidecarIt->second->bytes.data(), sizeof(meta));
  EXPECT_EQ(meta.wordCount, 65450u);
  EXPECT_EQ(meta.wordCountIsEstimate, 0);  // author-reported (from OPF), not counted on-device

  const auto indexIt = fake::files.find(kIndexPath);
  ASSERT_NE(indexIt, fake::files.end());
  CompactIndexRecord rec;
  const uint32_t off = offsetOf(0);
  ASSERT_GE(indexIt->second->bytes.size(), off + sizeof(rec));
  std::memcpy(&rec, indexIt->second->bytes.data() + off, sizeof(rec));
  EXPECT_STREQ(rec.relationship1, "Hermione Granger/Draco Malfoy");
  EXPECT_EQ(rec.wordCount, 65450u);
}

// The companion "both absent" case: confirmed via the same device-library survey to be the
// majority outcome (68% of real files), and the user's explicit decision was to leave this
// blank rather than add a targeted HTML fallback -- so this pins "stays blank, doesn't crash
// or fabricate a value" as the expected behavior, not an oversight to fix later.
TEST_F(Ao3LibrarianScrapeGuardTest, NoOpfShipDataLeavesRelationshipBlank) {
  Epub epub("/Books/Little Hearth Fires.epub", "/.crosspoint");
  epub.spineItemsCount = 1;
  epub.fakeTitlePageHtml = R"(<h1><a href="https://archiveofourown.org/works/23725228">Little Hearth Fires</a></h1>)";
  epub.wordCountValue = "4994";
  // mainShipValue/otherShipsValue left empty -- the confirmed-common case.

  ASSERT_TRUE(Ao3Librarian::scrape(epub, /*force=*/true));

  const auto indexIt = fake::files.find(kIndexPath);
  ASSERT_NE(indexIt, fake::files.end());
  CompactIndexRecord rec;
  const uint32_t off = offsetOf(0);
  ASSERT_GE(indexIt->second->bytes.size(), off + sizeof(rec));
  std::memcpy(&rec, indexIt->second->bytes.data() + off, sizeof(rec));
  EXPECT_STREQ(rec.relationship1, "");
  EXPECT_STREQ(rec.relationship2, "");
  EXPECT_EQ(rec.wordCount, 4994u);  // word count still enriched independently of pairing
}

// Same cascade, for series info: the condensed template has no "Part N of ..." text either,
// so parseTitlePage() leaves meta->seriesPart at 0 -- the same trust signal the fallback
// above checks for wordCount/ships. getSeriesIndex()/getSeriesName() simulate what Calibre's
// native series fields would supply for such a file.
TEST_F(Ao3LibrarianScrapeGuardTest, OpfSuppliesSeriesPartTheCondensedTemplateCannot) {
  Epub epub("/Books/Possibilities Part 4.epub", "/.crosspoint");
  epub.spineItemsCount = 1;
  epub.fakeTitlePageHtml = R"(<h1><a href="https://archiveofourown.org/works/11111111">Part Four</a></h1>)";
  epub.seriesIndexValue = "4.0";
  epub.seriesNameValue = "Possibilities";

  ASSERT_TRUE(Ao3Librarian::scrape(epub, /*force=*/true));

  const auto indexIt = fake::files.find(kIndexPath);
  ASSERT_NE(indexIt, fake::files.end());
  CompactIndexRecord rec;
  const uint32_t off = offsetOf(0);
  ASSERT_GE(indexIt->second->bytes.size(), off + sizeof(rec));
  std::memcpy(&rec, indexIt->second->bytes.data() + off, sizeof(rec));
  EXPECT_EQ(rec.seriesPart, 4);
  EXPECT_STREQ(rec.seriesName, "Possibilities");
}

// Coverage for writeIndexRecord()'s own flags-preservation fix: bit 0 (tombstone) and bit 1
// ("finished", set by setRecordFinished() via its own direct read-modify-write, entirely
// separate from scrape()) have no source in a fresh scrape -- it always passes flags=0 -- so
// an update must not clobber what already lived in the matched slot.
TEST_F(Ao3LibrarianScrapeGuardTest, RescrapePreservesTheFinishedFlagAnExistingRecordHad) {
  Epub epub("/Books/Hard Lines.epub", "/.crosspoint");
  epub.spineItemsCount = 1;
  epub.fakeTitlePageHtml = R"(<h1><a href="https://archiveofourown.org/works/59081659">Hard Lines</a></h1>)";
  epub.wordCountValue = "65450";

  // Seed an existing AO3X v4 index with one live record for this exact fic, already marked
  // "finished" by the user.
  CompactIndexRecord existing{};
  existing.cacheHash = ZipFile::fnvHash64(epub.getPath().c_str(), epub.getPath().size());
  existing.flags = 0x02;
  std::string blob;
  blob.append("AO3X", 4);
  const uint8_t version = 4;
  blob.append(reinterpret_cast<const char*>(&version), 1);
  const uint16_t recordCount = 1;
  blob.append(reinterpret_cast<const char*>(&recordCount), sizeof(recordCount));
  const uint32_t nextSequence = 7;
  blob.append(reinterpret_cast<const char*>(&nextSequence), sizeof(nextSequence));
  const uint8_t reserved = 0;
  blob.append(reinterpret_cast<const char*>(&reserved), 1);
  blob.append(reinterpret_cast<const char*>(&existing), sizeof(existing));
  fake::add(kIndexPath, blob);

  ASSERT_TRUE(Ao3Librarian::scrape(epub, /*force=*/true));

  const auto indexIt = fake::files.find(kIndexPath);
  ASSERT_NE(indexIt, fake::files.end());
  CompactIndexRecord rec;
  const uint32_t off = offsetOf(0);
  ASSERT_GE(indexIt->second->bytes.size(), off + sizeof(rec));
  std::memcpy(&rec, indexIt->second->bytes.data() + off, sizeof(rec));
  EXPECT_EQ(rec.flags & 0x02, 0x02);
  EXPECT_EQ(rec.wordCount, 65450u);  // fresh scrape data still applied on top
}

}  // namespace
