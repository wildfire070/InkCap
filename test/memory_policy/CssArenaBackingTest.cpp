#include <Arduino.h>
#include <CssParser.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

struct CssArenaBackingTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  void createCache() {
    const std::string text =
        "p { text-align: center; margin-top: 12px; font-size: 150%; } .em { font-weight: bold; } div p { font-style: "
        "italic; } "
        ".plain { list-style-type: none; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    ASSERT_TRUE(css.saveToCache());
  }
  void checkStyle(CssParser& css) {
    EXPECT_FALSE(css.empty());
    EXPECT_EQ(css.ruleCount(), 3u);
    const auto style = css.resolveStyle("p", "em", {{0, "div", ""}});
    EXPECT_TRUE(style.hasFontWeight());
    EXPECT_TRUE(style.hasFontStyle());
    EXPECT_TRUE(style.hasTextAlign());
    EXPECT_TRUE(style.hasMarginTop());
    EXPECT_EQ(style.marginTop.value, 12);
    ASSERT_TRUE(style.hasFontSize());
    EXPECT_EQ(style.fontSize.value, 150);
    EXPECT_EQ(style.fontSize.unit, CssUnit::Percent);
    EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
    EXPECT_EQ(style.textAlign, CssTextAlign::Center);
    const auto listStyle = css.resolveStyle("ol", "plain");
    EXPECT_TRUE(listStyle.hasListStyleType());
    EXPECT_EQ(listStyle.listStyleType, CssListStyleType::None);
  }
};
TEST_F(CssArenaBackingTest, IdenticalStylesForDefaultExternalAndDiskFallback) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    createCache();
    CssParser css("book");
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    checkStyle(css);
    if (mode == 2)
      EXPECT_TRUE(fakeheap::live.empty());
    else {
      ASSERT_EQ(fakeheap::live.size(), 1u);
      EXPECT_EQ(fakeheap::live.begin()->second.external, mode == 1);
    }
    css.clear();
    EXPECT_TRUE(css.empty());
    EXPECT_EQ(css.ruleCount(), 0u);
  }
}
TEST_F(CssArenaBackingTest, ExternalHydrationPreservesInternalAndExternalReserves) {
  createCache();
  for (bool internalPressure : {false, true}) {
    fakeheap::reset();
    CssParser css("book");
    if (internalPressure)
      fakeheap::internal.free = 79 * 1024;
    else
      fakeheap::external.free = 128 * 1024;
    ASSERT_TRUE(css.loadFromCache());
    checkStyle(css);
    EXPECT_TRUE(fakeheap::live.empty());
  }
}

struct CssDescendantDepthTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  // ".fff_titlepage .title h1" (3-part) and ".fff_titlepage dl .inline dd"
  // (4-part) mirror the real selectors found in FanFicFare's stylesheet.
  static constexpr const char* kCss =
      ".fff_titlepage .title h1 { font-weight: normal; } "
      ".fff_titlepage dl .inline dd { text-decoration: underline; }";
  // Populates `out` by round-tripping kCss through saveToCache/loadFromCache
  // (CssParser is non-copyable and non-movable, so this takes an out-param
  // rather than returning by value).
  void buildAndRoundTripThroughCache(CssParser& out) {
    Storage.put("input.css", {std::string(kCss).begin(), std::string(kCss).end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser writer("book");
    ASSERT_TRUE(writer.loadFromStream(file));
    file.close();
    ASSERT_TRUE(writer.saveToCache());
    ASSERT_TRUE(out.loadFromCache());
  }
};

TEST_F(CssDescendantDepthTest, ThreePartSelectorAppliesWhenBothContextPartsPresent) {
  CssParser css("book");
  buildAndRoundTripThroughCache(css);
  const auto style = css.resolveStyle("h1", "", {{0, "body", "fff_titlepage"}, {1, "div", "title"}});
  EXPECT_TRUE(style.hasFontWeight());
  EXPECT_EQ(style.fontWeight, CssFontWeight::Normal);
}

TEST_F(CssDescendantDepthTest, ThreePartSelectorDoesNotApplyWithOnlyOneContextPartPresent) {
  CssParser css("book");
  buildAndRoundTripThroughCache(css);
  // Only ".title" is present in the ancestor stack; ".fff_titlepage" is missing.
  const auto style = css.resolveStyle("h1", "", {{0, "div", "title"}});
  EXPECT_FALSE(style.hasFontWeight());
}

TEST_F(CssDescendantDepthTest, FourPartSelectorAppliesWhenAllContextPartsPresent) {
  CssParser css("book");
  buildAndRoundTripThroughCache(css);
  const auto style =
      css.resolveStyle("dd", "", {{0, "body", "fff_titlepage"}, {1, "dl", ""}, {2, "div", "inline"}});
  EXPECT_TRUE(style.hasTextDecoration());
  EXPECT_EQ(style.textDecoration, CssTextDecoration::Underline);
}

TEST(CssBorderPropertyTest, BorderShorthandSetsAllFourSides) {
  const auto style = CssParser::parseInlineStyle("border: 1px solid #000");
  EXPECT_TRUE(style.hasBorderTop());
  EXPECT_TRUE(style.hasBorderRight());
  EXPECT_TRUE(style.hasBorderBottom());
  EXPECT_TRUE(style.hasBorderLeft());
  EXPECT_TRUE(style.borderTop);
  EXPECT_TRUE(style.borderRight);
  EXPECT_TRUE(style.borderBottom);
  EXPECT_TRUE(style.borderLeft);
}

TEST(CssBorderPropertyTest, ZeroWidthBorderIsNotPresent) {
  const auto style = CssParser::parseInlineStyle("border: 0");
  EXPECT_TRUE(style.hasBorderTop());
  EXPECT_FALSE(style.borderTop);
}

TEST(CssBorderPropertyTest, NoneKeywordIsNotPresent) {
  const auto style = CssParser::parseInlineStyle("border-bottom: none");
  EXPECT_TRUE(style.hasBorderBottom());
  EXPECT_FALSE(style.borderBottom);
}

TEST(CssBorderPropertyTest, PerSidePropertiesAreIndependent) {
  // Mirrors the real "blockquote{border-left:0.5px solid #9b9b9b}" case.
  const auto style = CssParser::parseInlineStyle("border-left: 0.5px solid #9b9b9b");
  EXPECT_TRUE(style.hasBorderLeft());
  EXPECT_TRUE(style.borderLeft);
  EXPECT_FALSE(style.hasBorderTop());
  EXPECT_FALSE(style.hasBorderRight());
  EXPECT_FALSE(style.hasBorderBottom());
}

TEST(CssBorderPropertyTest, StyleOnlyValueWithoutExplicitWidthIsPresent) {
  const auto style = CssParser::parseInlineStyle("border-top: solid");
  EXPECT_TRUE(style.hasBorderTop());
  EXPECT_TRUE(style.borderTop);
}

TEST(CssFontSizePropertyTest, EmValueSetsExactMultiplier) {
  // Mirrors the real ".fff_titlepage .title h1 { font-size: 1.75em; }" case.
  const auto style = CssParser::parseInlineStyle("font-size: 1.75em");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(style.fontSizeMultiplier, 1.75f);
}

TEST(CssFontSizePropertyTest, PercentValueDividesBy100) {
  const auto style = CssParser::parseInlineStyle("font-size: 125%");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(style.fontSizeMultiplier, 1.25f);
}

TEST(CssFontSizePropertyTest, PxValueDividesBy16) {
  const auto style = CssParser::parseInlineStyle("font-size: 20px");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(style.fontSizeMultiplier, 1.25f);
}

TEST(CssFontSizePropertyTest, SmallerKeywordFoldsOntoSmall) {
  const auto style = CssParser::parseInlineStyle("font-size: smaller");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(style.fontSizeMultiplier, 0.8f);
}

TEST(CssFontSizePropertyTest, LargerKeywordFoldsOntoLarge) {
  const auto style = CssParser::parseInlineStyle("font-size: larger");
  ASSERT_TRUE(style.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(style.fontSizeMultiplier, 1.2f);
}

TEST(CssFontSizePropertyTest, InvalidKeywordIsNotSet) {
  const auto style = CssParser::parseInlineStyle("font-size: inherit");
  EXPECT_FALSE(style.hasFontSizeMultiplier());
}

TEST_F(CssDescendantDepthTest, SixPartSelectorMatchesArbitrarilyDeepAncestorChains) {
  // contextMatches() walks its ancestor-selector prefix iteratively rather than against a
  // fixed-size parts array, so a 6-part selector (5 context parts + subject) is not a special
  // case -- it matches exactly like the 3- and 4-part selectors above, with all 5 ancestors
  // present and in order.
  const std::string css6Parts = "a b c d e f { font-weight: bold; }";  // 5 context parts + subject "f"
  Storage.put("six.css", {css6Parts.begin(), css6Parts.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "six.css", file));
  CssParser parser("book6");
  ASSERT_TRUE(parser.loadFromStream(file));
  file.close();
  EXPECT_FALSE(parser.empty());
  const auto style =
      parser.resolveStyle("f", "", {{0, "a", ""}, {1, "b", ""}, {2, "c", ""}, {3, "d", ""}, {4, "e", ""}});
  EXPECT_TRUE(style.hasFontWeight());
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
}

TEST_F(CssArenaBackingTest, LargerSourceAllowanceRequiresPsram) {
  EXPECT_EQ(CssParser::maxSourceBytes(), 512u * 1024u);
  fakeheap::reset(false);
  EXPECT_EQ(CssParser::maxSourceBytes(), 128u * 1024u);
}

TEST_F(CssArenaBackingTest, LargeStreamPreservesHiddenRulesThroughCache) {
  // Publisher comments can make a stylesheet exceed the old 128 KiB limit
  // without needing a large rule map. The parser must reach the hidden rule.
  const std::string text = "/*" + std::string(191327, ' ') + "*/\n.modal { display: none; }";
  ASSERT_LT(text.size(), CssParser::maxSourceBytes());
  Storage.put("large.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "large.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
}

TEST_F(CssArenaBackingTest, PreviousCacheVersionIsInvalidated) {
  CssParser css("book");
  ASSERT_TRUE(css.saveToCache());
  FsFile file;
  // Obtain a valid empty cache, then mark it as the prior cache revision.
  ASSERT_TRUE(Storage.openFileForRead("test", "book/css_rules.cache", file));
  std::vector<uint8_t> bytes(file.size());
  ASSERT_EQ(file.read(bytes.data(), bytes.size()), static_cast<int>(bytes.size()));
  file.close();
  bytes[4] = CssParser::CSS_CACHE_VERSION - 1;
  Storage.put("book/css_rules.cache", bytes);
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
}

TEST_F(CssArenaBackingTest, PsramParsingMergesSelectorsAndReleasesAllSlabs) {
  const std::string text = ".modal { display: none; } .MODAL { font-weight: bold; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  EXPECT_EQ(css.ruleCount(), 1u);
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
  EXPECT_EQ(css.resolveStyle("div", "modal").fontWeight, CssFontWeight::Bold);
  ASSERT_FALSE(fakeheap::live.empty());
  for (const auto& allocation : fakeheap::live) EXPECT_TRUE(allocation.second.external);
  css.clear();
  EXPECT_TRUE(fakeheap::live.empty());
}

TEST_F(CssArenaBackingTest, PsramParsingAllocationFailureStopsSafely) {
  const std::string text = ".modal { display: none; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  fakeheap::external.fail = 1;
  EXPECT_FALSE(css.loadFromStream(file));
  file.close();
  EXPECT_TRUE(css.empty());
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(fakeheap::internal.attempts, 0u);
}

TEST_F(CssArenaBackingTest, ManyRulesSurviveArenaGrowthAndCacheRoundTrip) {
  std::string text;
  for (int i = 0; i < 1303; ++i) text += ".rule" + std::to_string(i) + " { display: none; }\n";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  ASSERT_EQ(css.ruleCount(), 1303u);
  for (int i = 0; i < 1303; ++i)
    EXPECT_EQ(css.resolveStyle("div", "rule" + std::to_string(i)).display, CssDisplay::None);
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  ASSERT_EQ(css.ruleCount(), 1303u);
  for (int i = 0; i < 1303; ++i)
    EXPECT_EQ(css.resolveStyle("div", "rule" + std::to_string(i)).display, CssDisplay::None);
}

TEST_F(CssArenaBackingTest, BorderSuppressionSurvivesHydrationAndDiskFallback) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    const std::string text =
        "hr.transition { border: none; } hr.visible { border: none; border-top: 1px solid; } "
        "div hr { border-width: 0; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    EXPECT_TRUE(css.resolveStyle("hr", "transition").suppressesHorizontalRule());
    EXPECT_FALSE(css.resolveStyle("hr", "visible").suppressesHorizontalRule());
    ASSERT_TRUE(css.saveToCache());
    css.clear();
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    if (mode == 2) EXPECT_TRUE(fakeheap::live.empty());
    EXPECT_TRUE(css.resolveStyle("hr", "transition").suppressesHorizontalRule());
    EXPECT_FALSE(css.resolveStyle("hr", "visible").suppressesHorizontalRule());
    EXPECT_TRUE(css.resolveStyle("hr", "", {{0, "div", ""}}).suppressesHorizontalRule());
  }
}

TEST_F(CssArenaBackingTest, PublisherDecorationsAndContextSurviveAllCacheBackings) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    const std::string text =
        "section#chapter > p.note.wide { border: 2px dashed; background: #ddd; white-space: pre-wrap; } "
        "p.note::first-letter { initial-letter: 3; float: left; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    ASSERT_TRUE(css.saveToCache());
    css.clear();
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    auto style = css.resolveStyle("p", "wide note", {{0, "section", "", "chapter"}});
    EXPECT_EQ(style.borders[0].width, 2);
    EXPECT_EQ(style.borders[0].style, CssBorderStyle::Dashed);
    EXPECT_TRUE(style.shaded);
    EXPECT_TRUE(style.preserveWhitespace);
    auto cap = css.resolveStyle("p", "note", {}, {}, true);
    EXPECT_EQ(cap.initialLetter, 3);
    EXPECT_TRUE(cap.floatLeft);
  }
}
