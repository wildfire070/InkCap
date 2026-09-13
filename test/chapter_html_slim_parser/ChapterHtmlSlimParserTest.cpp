#include <Epub.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

TEST(ParagraphIndentTest, DoesNotInventIndentWithoutSourceCss) {
  GfxRenderer renderer;

  for (const bool extraParagraphSpacing : {false, true}) {
    ParsedText paragraph(extraParagraphSpacing);
    EXPECT_EQ(paragraph.resolveFirstLineIndent(true, renderer, 0), 0);
  }
}

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  Epub epub;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{epub,  filepath, renderer, 0,  1.0f, false, false, 0, 480, 800,     false,
                               false, false,    0,        {}, true, "",    "",    0, {},  nullptr, &cssParser};
  std::array<ChapterHtmlSlimParser::StyleStackEntry, 4> inlineStyles{};
  std::array<BlockStyle, 4> blockStyles{};

  void SetUp() override {
    parser.currentTextBlock = std::make_unique<ParsedText>(false);
    parser.inlineStyleBuf_ = inlineStyles.data();
    parser.blockStyleBuf_ = blockStyles.data();
    parser.blockStyleCount_ = 1;
  }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_NE(footnote.linkId, 0u);
  ASSERT_EQ(parser.currentTextBlock->wordBackgroundBlack.size(), 1u);
  const uint8_t wordLinkId =
      static_cast<uint8_t>((parser.currentTextBlock->wordBackgroundBlack.front() & TextBlock::WORD_FLAG_LINK_ID_MASK) >>
                           TextBlock::WORD_FLAG_LINK_ID_SHIFT);
  EXPECT_EQ(wordLinkId, footnote.linkId);
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, LegacyAlignAttributeAppliesWhenNoCssTextAlign) {
  // Some EPUB converters/editors still emit the legacy presentational
  // align="" HTML attribute instead of (or alongside) CSS -- confirmed on a
  // real AO3 fanfic chapter whose author's-note paragraphs used
  // <p align="center">/<p align="left"> with no matching CSS rule, which
  // rendered flat left-aligned on-device instead of the intended alternation.
  // paragraphAlignment=None ("Book's Style") is required for either the CSS
  // or this fallback to have any effect -- a forced reader alignment always
  // wins over both, same as CSS always wins over this fallback below.
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
  const XML_Char* attributes[] = {"align", "center", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment, CssTextAlign::Center);
}

TEST_F(ChapterHtmlSlimParserTest, CssTextAlignOverridesLegacyAlignAttribute) {
  // Real CSS/inline style must win over the legacy attribute, matching
  // browser cascade precedence -- the fallback only fills a gap, it never
  // competes with an actual style rule.
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
  const XML_Char* attributes[] = {"align", "center", "style", "text-align: left", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment, CssTextAlign::Left);
}

TEST_F(ChapterHtmlSlimParserTest, BodyTextAlignBecomesTheDefaultForPlainParagraphs) {
  // computeStyleForElement() only matches an element's own tag/class, not full
  // CSS inheritance -- confirmed on a real book (stylesheet.css sets
  // body{text-align:left}) whose plain <p>s rendered justified instead of
  // left-aligned on-device, since "Book's Style" mode's own Justify default
  // (see beginParse()) never saw body's override at all.
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
  const XML_Char* attributes[] = {"style", "text-align: left", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment, CssTextAlign::Left);
}

TEST_F(ChapterHtmlSlimParserTest, ParagraphsOwnCssTextAlignOverridesInheritedBodyAlignment) {
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
  const XML_Char* bodyAttributes[] = {"style", "text-align: left", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);
  const XML_Char* pAttributes[] = {"style", "text-align: center", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", pAttributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment, CssTextAlign::Center);
}

TEST_F(ChapterHtmlSlimParserTest, ForcedReaderAlignmentIgnoresBodyTextAlign) {
  // A forced reader alignment setting always wins, same as every other
  // Book's-Style-only override in this parser (align="", font-size ladder, etc).
  parser.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::Center);
  const XML_Char* attributes[] = {"style", "text-align: left", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().alignment, CssTextAlign::Center);
}

TEST_F(ChapterHtmlSlimParserTest, UsesOptimizerImageDimensionsWithoutReadingTheCompressedImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 800;
  epub.optimizerImageHeight = 7;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  EXPECT_EQ(epub.streamReadCount, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageImage);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 4);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenElementsSuppressContentAndResumeVisibleText) {
  for (const char* tag : {"p", "h1", "span", "div", "a", "table"}) {
    for (const char* value : {"hidden", "", "false"}) {
      const XML_Char* attributes[] = {"hidden", value, "style", "display: block", nullptr};
      ChapterHtmlSlimParser::startElement(&parser, tag, attributes);
      ChapterHtmlSlimParser::startElement(&parser, "span", nullptr);
      ChapterHtmlSlimParser::characterData(&parser, "HIDDEN ", 7);
      ChapterHtmlSlimParser::endElement(&parser, "span");
      ChapterHtmlSlimParser::endElement(&parser, tag);
      EXPECT_EQ(parser.currentTextBlock->size(), 0u);
      EXPECT_EQ(parser.partWordBufferIndex, 0);
    }
  }
  ChapterHtmlSlimParser::characterData(&parser, "Visible ", 8);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Visible");
}

TEST_F(ChapterHtmlSlimParserTest, HiddenImageDoesNotReadImageDataWithoutCss) {
  parser.cssParser = nullptr;
  const XML_Char* attributes[] = {"hidden", "", "src", "missing.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(parser.currentPage, nullptr);
}

TEST_F(ChapterHtmlSlimParserTest, BlockquoteBorderLeftProducesOneBorderBoxWithOnlyThatSide) {
  const XML_Char* attributes[] = {"style", "border-left: 0.5px solid #9b9b9b", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "Quoted text", 11);
  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);  // flush onto a page

  ASSERT_NE(parser.currentPage, nullptr);
  int borderBoxCount = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageCssBorderBox) continue;
    ++borderBoxCount;
    const auto& box = static_cast<const PageCssBorderBox&>(*element);
    EXPECT_TRUE(box.hasBorderLeft());
    EXPECT_FALSE(box.hasBorderTop());
    EXPECT_FALSE(box.hasBorderRight());
    EXPECT_FALSE(box.hasBorderBottom());
    EXPECT_GT(box.getHeight(), 0);
  }
  EXPECT_EQ(borderBoxCount, 1);
}

TEST_F(ChapterHtmlSlimParserTest, AllFourBorderSidesProduceOneBoxWithAllSidesSet) {
  const XML_Char* attributes[] = {"style", "border: 1px solid #000", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "Boxed text", 10);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);

  ASSERT_NE(parser.currentPage, nullptr);
  int borderBoxCount = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageCssBorderBox) continue;
    ++borderBoxCount;
    const auto& box = static_cast<const PageCssBorderBox&>(*element);
    EXPECT_TRUE(box.hasBorderTop());
    EXPECT_TRUE(box.hasBorderRight());
    EXPECT_TRUE(box.hasBorderBottom());
    EXPECT_TRUE(box.hasBorderLeft());
  }
  EXPECT_EQ(borderBoxCount, 1);
}

TEST_F(ChapterHtmlSlimParserTest, BlockWithoutBorderProducesNoBorderBox) {
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Plain quote", 11);
  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);

  ASSERT_NE(parser.currentPage, nullptr);
  for (const auto& element : parser.currentPage->elements) {
    EXPECT_NE(element->getTag(), TAG_PageCssBorderBox);
  }
}

// A bordered block whose content is forced to split across a page break must
// produce two independent PageCssBorderBox fragments (one per page), each
// covering only that page's portion of the block -- not one box that somehow
// spans the break, and not a crash/corruption from the depth-matched stack.
TEST_F(ChapterHtmlSlimParserTest, BorderBoxSplitAcrossAPageBreakProducesTwoIndependentFragments) {
  std::vector<std::unique_ptr<Page>> completedPages;
  parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
    completedPages.push_back(std::move(page));
  };
  // The stub GfxRenderer measures every glyph/word at 0px width, so pixel-width
  // wrapping never kicks in here -- force multiple lines the way real HTML
  // does regardless of measured width, via explicit <br/>. getLineHeight() is
  // fixed at 16px, so a 32px viewport fits exactly 2 such lines before a 3rd
  // forces a break.
  parser.viewportHeight = 32;

  const XML_Char* attributes[] = {"style", "border-left: 1px solid #000", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", attributes);
  for (int i = 0; i < 5; ++i) {
    ChapterHtmlSlimParser::characterData(&parser, "word", 4);
    ChapterHtmlSlimParser::startElement(&parser, "br", nullptr);
    ChapterHtmlSlimParser::endElement(&parser, "br");
  }
  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);  // flush trailing content

  ASSERT_GE(completedPages.size(), 1u) << "test setup should have forced at least one page break";

  // The border box on the (now-completed) first page should be finalized
  // with a positive height and only the declared side set.
  int firstPageBoxCount = 0;
  for (const auto& element : completedPages.front()->elements) {
    if (element->getTag() != TAG_PageCssBorderBox) continue;
    ++firstPageBoxCount;
    const auto& box = static_cast<const PageCssBorderBox&>(*element);
    EXPECT_TRUE(box.hasBorderLeft());
    EXPECT_GT(box.getHeight(), 0);
  }
  EXPECT_EQ(firstPageBoxCount, 1);

  // The current (new) page should have its own independent border box too.
  ASSERT_NE(parser.currentPage, nullptr);
  int currentPageBoxCount = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageCssBorderBox) continue;
    ++currentPageBoxCount;
  }
  EXPECT_EQ(currentPageBoxCount, 1);
}

// The FanFicFare ".hr-sect" divider (see BlockStyle::hrSectDivider /
// PageHrSectRule): CssParser doesn't support the pseudo-elements/flexbox the
// real CSS draws its flanking lines with, so ChapterHtmlSlimParser detects
// the class name directly and reproduces the visual result from the
// already-centered line's own word position instead.
//
// This test target's stub GfxRenderer measures every word at 0px, which
// (unrelated to hr-sect) makes the real word-wrap/line-extraction pipeline
// produce lines with no recoverable word data here -- so unlike the CSS
// border-box tests, this checks the block-style detection directly rather
// than the resulting PageHrSectRule's geometry. The actual rule-drawing
// geometry is verified visually against a real book with EpubRenderPreview
// (real fonts, real widths) -- see the plan's Verification section.
TEST_F(ChapterHtmlSlimParserTest, HrSectClassSetsBlockStyleFlag) {
  const XML_Char* attributes[] = {"class", "hr-sect", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_TRUE(parser.currentTextBlock->getBlockStyle().hrSectDivider);
}

TEST_F(ChapterHtmlSlimParserTest, PlainDivDoesNotSetHrSectFlag) {
  ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_FALSE(parser.currentTextBlock->getBlockStyle().hrSectDivider);
}

TEST_F(ChapterHtmlSlimParserTest, UnrelatedClassDoesNotSetHrSectFlag) {
  // hasClassToken() must be word-boundary-safe: a class that merely contains
  // "hr-sect" as a substring must not match.
  const XML_Char* attributes[] = {"class", "not-hr-sect-related", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_FALSE(parser.currentTextBlock->getBlockStyle().hrSectDivider);
}

TEST_F(ChapterHtmlSlimParserTest, PlainDivProducesNoHrSectRule) {
  ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Just a div", 10);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);  // flush trailing content

  ASSERT_NE(parser.currentPage, nullptr);
  for (const auto& element : parser.currentPage->elements) {
    EXPECT_NE(element->getTag(), TAG_PageHrSectRule);
  }
}

// FanFicFare title pages list metadata as <dt>/<dd> pairs. Real-world markup
// marks a specific dd `display: inline` so its label ("Series:") and value
// stay on one line, while an otherwise-identical sibling dd with no such
// override (e.g. "Tags:") renders as its own block/paragraph -- see
// BlockDisplayDdStartsANewBlockFromItsDt below for the contrasting case.
// dt/dd are ordinary block tags otherwise (matching the HTML spec's suggested
// default rendering), so both must push/pop the block-style stack like any
// other BLOCK_TAGS member unless CSS says display:inline.
TEST_F(ChapterHtmlSlimParserTest, InlineDisplayDdContinuesTheSameBlockAsItsDt) {
  ChapterHtmlSlimParser::startElement(&parser, "dt", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Series:", 7);
  ChapterHtmlSlimParser::endElement(&parser, "dt");

  const XML_Char* attributes[] = {"style", "display: inline", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "dd", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "Value", 5);
  ChapterHtmlSlimParser::endElement(&parser, "dd");

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->size(), 2u);
}

TEST_F(ChapterHtmlSlimParserTest, BlockDisplayDdStartsANewBlockFromItsDt) {
  ChapterHtmlSlimParser::startElement(&parser, "dt", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Tags:", 5);
  ChapterHtmlSlimParser::endElement(&parser, "dt");

  ChapterHtmlSlimParser::startElement(&parser, "dd", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Explicit", 8);
  ChapterHtmlSlimParser::endElement(&parser, "dd");

  // The dt's "Tags:" was flushed into its own page/block when dd opened a new
  // one, so the current block should hold only dd's own word.
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, InlineBlockDisplayAlsoCountsAsInlineForDd) {
  ChapterHtmlSlimParser::startElement(&parser, "dt", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Series:", 7);
  ChapterHtmlSlimParser::endElement(&parser, "dt");

  const XML_Char* attributes[] = {"style", "display: inline-block", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "dd", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "Value", 5);
  ChapterHtmlSlimParser::endElement(&parser, "dd");

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->size(), 2u);
}

TEST_F(ChapterHtmlSlimParserTest, FontSizeResolvesToNearestLadderRungWhenExactMatch) {
  parser.fontSizeLadder_.addRung(999, 175);  // fake "175% of body" rung

  const XML_Char* attributes[] = {"style", "font-size: 1.75em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().headingFontId, 999);
}

TEST_F(ChapterHtmlSlimParserTest, FontSizeWithNoLadderMatchFallsBackToResidualScale) {
  // With an EMPTY ladder (e.g. an SD-card body font, whose id never matches a
  // built-in family's rungs) there is no real font resource to snap to, so
  // the body font stays the render font (headingFontId == 0) -- but rather
  // than silently dropping font-size entirely, the desired 175% is kept as a
  // residual scale that GfxRenderer::drawTextScaled() resamples at render time.
  const XML_Char* attributes[] = {"style", "font-size: 1.75em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);

  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().headingFontId, 0);
  EXPECT_FLOAT_EQ(parser.currentTextBlock->getBlockStyle().fontSizeResidualScale, 1.75f);
}

TEST_F(ChapterHtmlSlimParserTest, FontSizeResidualScaleIsClampedToSaneRange) {
  // A pathological CSS value (way beyond any real heading/pre use) must not
  // produce illegibly tiny or oversized resampled text.
  const XML_Char* hugeAttrs[] = {"style", "font-size: 6em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", hugeAttrs);
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_FLOAT_EQ(parser.currentTextBlock->getBlockStyle().fontSizeResidualScale, 2.0f);
  ChapterHtmlSlimParser::endElement(&parser, "h1");

  const XML_Char* tinyAttrs[] = {"style", "font-size: 0.1em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", tinyAttrs);
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_FLOAT_EQ(parser.currentTextBlock->getBlockStyle().fontSizeResidualScale, 0.6f);
}

TEST_F(ChapterHtmlSlimParserTest, SecondDifferentlySizedBlockFallsBackOnceAuxSlotIsClaimed) {
  parser.fontSizeLadder_.addRung(111, 150);
  parser.fontSizeLadder_.addRung(222, 200);

  const XML_Char* firstAttrs[] = {"style", "font-size: 1.5em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", firstAttrs);
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().headingFontId, 111);
  EXPECT_EQ(parser.auxFontId_, 111);
  ChapterHtmlSlimParser::endElement(&parser, "h1");

  const XML_Char* secondAttrs[] = {"style", "font-size: 2em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", secondAttrs);
  ASSERT_NE(parser.currentTextBlock, nullptr);
  // 200% would resolve to fontId 222, but the one aux slot is already
  // claimed by 111 -- this block must keep the body font, not claim a second.
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().headingFontId, 0);
  EXPECT_EQ(parser.auxFontId_, 111);
}

TEST_F(ChapterHtmlSlimParserTest, LayoutWidthForBlockNarrowsBudgetByResidualScale) {
  // Laying out UNSCALED text against width/scale, then rendering the result
  // scaled, is exactly equivalent to laying it out at native scale -- this is
  // the whole mechanism that lets residual-scale rendering reuse ParsedText's
  // word-wrap/hyphenation/ruby code completely unmodified. Verify the math
  // directly since the test harness's stub GfxRenderer always measures text
  // at zero width, so wrap-overflow can't be observed through a real layout.
  BlockStyle enlarging;
  enlarging.fontSizeResidualScale = 1.75f;
  EXPECT_EQ(parser.layoutWidthForBlock(enlarging, 350), 200);  // 350 / 1.75

  BlockStyle shrinking;
  shrinking.fontSizeResidualScale = 0.5f;
  EXPECT_EQ(parser.layoutWidthForBlock(shrinking, 100), 200);  // 100 / 0.5

  // A block that resolved to a real ladder font renders at native size via
  // that font's own glyphs -- no width adjustment, regardless of scale.
  BlockStyle ladderResolved;
  ladderResolved.headingFontId = 42;
  ladderResolved.fontSizeResidualScale = 1.75f;
  EXPECT_EQ(parser.layoutWidthForBlock(ladderResolved, 350), 350);

  EXPECT_EQ(parser.layoutWidthForBlock(BlockStyle{}, 350), 350);
}

TEST_F(ChapterHtmlSlimParserTest, OrderedListItemsAreNumbered) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_GE(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_GE(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::endElement(&parser, "ol");
}

TEST_F(ChapterHtmlSlimParserTest, UnorderedListItemsKeepTheBullet) {
  ChapterHtmlSlimParser::startElement(&parser, "ul", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_GE(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "\xe2\x80\xa2");
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");
}

TEST_F(ChapterHtmlSlimParserTest, LiWithNoListAncestorKeepsTheBullet) {
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_GE(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "\xe2\x80\xa2");
  ChapterHtmlSlimParser::endElement(&parser, "li");
}

TEST_F(ChapterHtmlSlimParserTest, NestedOrderedListRestartsItsOwnNumbering) {
  ChapterHtmlSlimParser::startElement(&parser, "ul", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_GE(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ol");

  // Back in the outer <ul>, a sibling <li> keeps using bullets.
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_GE(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "\xe2\x80\xa2");
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");
}

TEST_F(ChapterHtmlSlimParserTest, SummaryStartsANewBlockInsteadOfContinuingInline) {
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Before");
  const ParsedText* beforeBlock = parser.currentTextBlock.get();

  // Opening <summary> here must start a genuinely NEW block (a fresh
  // ParsedText, flushing "Before" via makePages -> addLineToPage), not treat
  // <summary> as inline text that would run together with "Before".
  ChapterHtmlSlimParser::startElement(&parser, "summary", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Note", 4);
  ChapterHtmlSlimParser::endElement(&parser, "summary");

  EXPECT_NE(parser.currentTextBlock.get(), beforeBlock);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Note");
}

TEST_F(ChapterHtmlSlimParserTest, HiddenIdsDoNotBecomeAnchorsOrTocPageBreaks) {
  parser.tocAnchors.push_back("hidden-chapter");
  const XML_Char* idFirst[] = {"id", "hidden-chapter", "hidden", "hidden", nullptr};
  const XML_Char* hiddenFirst[] = {"hidden", "", "id", "hidden-chapter", nullptr};
  for (auto* attributes : {idFirst, hiddenFirst}) {
    ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
    ChapterHtmlSlimParser::characterData(&parser, "Hidden", 6);
    ChapterHtmlSlimParser::endElement(&parser, "h1");
    EXPECT_TRUE(parser.pendingAnchorId.empty());
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    EXPECT_TRUE(parser.anchorData.empty());
    EXPECT_EQ(parser.completedPageCount, 0);
    ChapterHtmlSlimParser::endElement(&parser, "p");
  }
}

}  // namespace
