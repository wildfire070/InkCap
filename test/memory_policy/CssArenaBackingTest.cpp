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
        "p { text-align: center; margin-top: 12px; } .em { font-weight: bold; } div p { font-style: italic; }";
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
    EXPECT_EQ(css.ruleCount(), 2u);
    const auto style = css.resolveStyle("p", "em", {{0, "div", ""}});
    EXPECT_TRUE(style.hasFontWeight());
    EXPECT_TRUE(style.hasFontStyle());
    EXPECT_TRUE(style.hasTextAlign());
    EXPECT_TRUE(style.hasMarginTop());
    EXPECT_EQ(style.marginTop.value, 12);
    EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
    EXPECT_EQ(style.textAlign, CssTextAlign::Center);
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

TEST_F(CssDescendantDepthTest, FivePlusPartSelectorIsRejectedNotMismatched) {
  // A 5-part selector (4 context parts + subject) exceeds MAX_DESCENDANT_CONTEXT_PARTS's
  // "4 context parts" budget only when it has 5 context parts (6 total) -- but a selector
  // with exactly 5 total parts (4 context + 1 subject) IS the supported boundary. Confirm a
  // 6-total-part selector (5 context parts) is silently dropped rather than partially matched.
  const std::string css6Parts =
      "a b c d e f { font-weight: bold; }";  // 5 context parts + subject "f"
  Storage.put("six.css", {css6Parts.begin(), css6Parts.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "six.css", file));
  CssParser parser("book6");
  ASSERT_TRUE(parser.loadFromStream(file));
  file.close();
  EXPECT_TRUE(parser.empty());
  const auto style =
      parser.resolveStyle("f", "", {{0, "a", ""}, {1, "b", ""}, {2, "c", ""}, {3, "d", ""}, {4, "e", ""}});
  EXPECT_FALSE(style.hasFontWeight());
}
