#include <gtest/gtest.h>

#include <string>

#include "ContentOpfParser.h"

namespace {

void parse(ContentOpfParser& parser, const std::string& xml) {
  ASSERT_TRUE(parser.setup());
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
}

}  // namespace

TEST(ContentOpfParserMetadata, EntityCallbackDoesNotSplitOneAuthor) {
  const std::string xml =
      R"(<package xmlns:dc="urn:dc"><metadata><dc:creator>&#201;mile Zola</dc:creator></metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.author, "Émile Zola");
}

TEST(ContentOpfParserMetadata, ClampsOversizedMetadataTextInsteadOfGrowingUnbounded) {
  const std::string hugeTitle(64 * 1024, 'A');
  const std::string xml =
      "<package xmlns:dc=\"urn:dc\"><metadata><dc:title>" + hugeTitle + " tail</dc:title></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title.size(), 512u);
  EXPECT_EQ(parser.title[0], 'A');
}

TEST(ContentOpfParserMetadata, ClampDoesNotSplitUtf8Codepoints) {
  const std::string prefix(511, 'A');
  const std::string xml = "<package xmlns:dc=\"urn:dc\"><metadata><dc:title>" + prefix +
                          "\xC3\xA9&amp;tail</dc:title></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, prefix);
}

TEST(ContentOpfParserMetadata, ClampNeverOvershootsAtAMultiCreatorSeparatorBoundary) {
  // First creator fills the field to exactly one byte under the cap, so the
  // second creator's leading ", " separator is the thing that would push the
  // total past 512 if the clamp checked only the next character.
  const std::string firstAuthor(511, 'A');
  const std::string xml = "<package xmlns:dc=\"urn:dc\"><metadata><dc:creator>" + firstAuthor +
                          "</dc:creator><dc:creator>B</dc:creator></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_LE(parser.author.size(), 512u);
  EXPECT_EQ(parser.author, firstAuthor);
}

TEST(ContentOpfParserMetadata, SeparatesCreatorElementsAndCollapsesXmlWhitespace) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>  The
   Left Hand   of Darkness  </dc:title>
    <dc:creator> Ursula   K. Le Guin </dc:creator>
    <dc:creator>
Octavia E. Butler
</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "The Left Hand of Darkness");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin, Octavia E. Butler");
}

TEST(ContentOpfParserMetadata, StopsBeforeManifestWithoutOpeningTemporaryStorage) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Wizard of Earthsea</dc:title>
    <dc:creator>Ursula K. Le Guin</dc:creator>
    <dc:language>en</dc:language>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, /*collectCssFiles=*/true,
                          /*metadataOnly=*/true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.title, "A Wizard of Earthsea");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, NeverEntersManifestWhenMetadataElementIsMissing) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="chapter.xhtml"/></manifest><spine/></package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, /*collectCssFiles=*/true,
                          /*metadataOnly=*/true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, MetadataOnlyStillCapturesFieldsThatDoNotNeedTheManifest) {
  // Epub::loadMetadata() copies these out of the parser and returns before the manifest-
  // dependent fields (cover href, TOC/guide) are even reachable -- everything asserted here
  // must already be fully parsed by the time </metadata> closes, or that fast path silently
  // drops it for every book the Library index builder scans.
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Wizard of Earthsea</dc:title>
    <dc:creator>Ursula K. Le Guin</dc:creator>
    <dc:identifier>https://archiveofourown.org/works/12345678</dc:identifier>
    <dc:subject>Fantasy</dc:subject>
    <dc:subject>Completed</dc:subject>
    <meta name="calibre:series" content="Earthsea"/>
    <meta name="calibre:series_index" content="1"/>
    <meta name="calibre:user_metadata:#completionstatus" content="{&quot;#value#&quot;: &quot;Complete&quot;}"/>
    <meta name="calibre:user_metadata:#like" content="{&quot;#value#&quot;: true}"/>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, /*collectCssFiles=*/true,
                          /*metadataOnly=*/true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.ao3WorkId, "12345678");
  EXPECT_TRUE(parser.ao3IsCompleted);
  EXPECT_EQ(parser.tags, "Fantasy, Completed");
  EXPECT_EQ(parser.seriesName, "Earthsea");
  EXPECT_EQ(parser.seriesIndex, "1");
  EXPECT_EQ(parser.completionStatus, "Complete");
  EXPECT_TRUE(parser.liked);
}

TEST(ContentOpfParserMetadata, CapturesBothBookIdsFromACalibreExportedFanFicFareOpf) {
  // Real shape of a FanFicFare epub exported through Calibre with the BookFusion plugin.
  const std::string xml = R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:title>Hard Lines</dc:title>
    <dc:identifier id="fanficfare-uid">fanficfare-uid:archiveofourown.org-ufyrelight-s59081659</dc:identifier>
    <dc:source>https://archiveofourown.org/works/59081659</dc:source>
    <dc:identifier opf:scheme="calibre">48b70e73-484f-44da-b803-4e4f5f9448a8</dc:identifier>
    <dc:identifier opf:scheme="BOOKFUSION">4883231</dc:identifier>
    <dc:identifier opf:scheme="URL">https://archiveofourown.org/works/59081659</dc:identifier>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.ao3WorkId, "59081659");
  EXPECT_EQ(parser.bookFusionId, 4883231u);
}

TEST(ContentOpfParserMetadata, BookFusionIdIsCaseInsensitiveAndKeepsOtherIdentifiersOut) {
  const std::string xml = R"(<package xmlns:dc="urn:dc" xmlns:opf="urn:opf"><metadata>
    <dc:identifier opf:scheme="bookfusion">bookfusion:777</dc:identifier>
    <dc:identifier opf:scheme="ISBN">9781234567897</dc:identifier>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.bookFusionId, 777u);
  EXPECT_TRUE(parser.ao3WorkId.empty());
}

TEST(ContentOpfParserMetadata, NoBookIdsWhenTheOpfHasNone) {
  const std::string xml =
      R"(<package xmlns:dc="urn:dc"><metadata><dc:identifier>urn:uuid:1234</dc:identifier></metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_TRUE(parser.ao3WorkId.empty());
  EXPECT_EQ(parser.bookFusionId, 0u);
}
