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

#include <Epub.h>
#include <MemoryBudget.h>

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
  std::array<ChapterHtmlSlimParser::StyleStackEntry, ChapterHtmlSlimParser::MAX_INLINE_STYLE_DEPTH> inlineStyles{};
  std::array<BlockStyle, ChapterHtmlSlimParser::MAX_BLOCK_STYLE_DEPTH> blockStyles{};

  void SetUp() override {
    GfxRenderer::loanActive = false;
    GfxRenderer::fileProbeHadLoan = false;
    MemoryBudget::imageAllowed = true;
    parser.currentTextBlock = std::make_unique<ParsedText>(false);
    parser.inlineStyleBuf_ = inlineStyles.data();
    parser.blockStyleBuf_ = blockStyles.data();
    parser.blockStyleCount_ = 1;
  }

  BlockStyle parseParagraph(const char* className) {
    const XML_Char* attributes[] = {"class", className, nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
    const BlockStyle style = parser.currentTextBlock->getBlockStyle();
    ChapterHtmlSlimParser::characterData(&parser, "Text", 4);
    ChapterHtmlSlimParser::endElement(&parser, "p");
    return style;
  }
};

TEST_F(ChapterHtmlSlimParserTest, BlockquoteParagraphsInheritItalicAndRestoreFollowingText) {
  cssParser.rulesBySelector_["blockquote"] =
      CssParser::parseInlineStyle("margin-left: 2em; margin-right: 1em; font-style: italic");
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", nullptr);

  for (int i = 0; i < 2; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
    ChapterHtmlSlimParser::characterData(&parser, "Quoted", 6);
    // Closing the paragraph must flush its final word before changing styles.
    ChapterHtmlSlimParser::endElement(&parser, "p");
    ASSERT_EQ(parser.currentTextBlock->size(), 1u);
    EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::ITALIC);
    EXPECT_TRUE(parser.effectiveItalic);
    EXPECT_EQ(parser.inlineStyleCount_, 1u);
  }

  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
  EXPECT_FALSE(parser.effectiveItalic);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Following", 9);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::REGULAR);
}

TEST_F(ChapterHtmlSlimParserTest, NestedBlocksOverrideAndRestoreInheritedBoldAndItalic) {
  const XML_Char* parentAttributes[] = {"style", "font-weight: bold; font-style: italic", nullptr};
  const XML_Char* normalAttributes[] = {"style", "font-weight: normal; font-style: normal", nullptr};
  const XML_Char* italicAttributes[] = {"style", "font-style: italic", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", parentAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", normalAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Normal ", 7);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0), EpdFontFamily::REGULAR);

  ChapterHtmlSlimParser::startElement(&parser, "span", italicAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Italic", 6);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ASSERT_EQ(parser.currentTextBlock->size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(1), EpdFontFamily::ITALIC);
  ChapterHtmlSlimParser::characterData(&parser, "Normal", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(2), EpdFontFamily::REGULAR);

  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Inherited", 9);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0),
            static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC));
  ChapterHtmlSlimParser::endElement(&parser, "div");
  EXPECT_FALSE(parser.effectiveBold);
  EXPECT_FALSE(parser.effectiveItalic);
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, NestedHeadingAndListPreserveBlockFontInheritance) {
  const XML_Char* attributes[] = {"style", "font-style: italic", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "h2", nullptr);
  EXPECT_TRUE(parser.effectiveItalic);
  ChapterHtmlSlimParser::characterData(&parser, "Heading", 7);
  ChapterHtmlSlimParser::endElement(&parser, "h2");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0),
            static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC));

  ChapterHtmlSlimParser::startElement(&parser, "ul", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Item", 4);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(1), EpdFontFamily::ITALIC);
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");
  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  EXPECT_FALSE(parser.effectiveItalic);
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, TableCellBlocksInheritOverrideAndRestoreFontStyles) {
  const XML_Char* parentAttributes[] = {"style", "font-weight: bold; font-style: italic", nullptr};
  const XML_Char* normalAttributes[] = {"style", "font-weight: normal; font-style: normal", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "td", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "blockquote", parentAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Quoted", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(0),
            static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::ITALIC));

  ChapterHtmlSlimParser::startElement(&parser, "p", normalAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Normal", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(1), EpdFontFamily::REGULAR);
  EXPECT_TRUE(parser.effectiveBold);
  EXPECT_TRUE(parser.effectiveItalic);

  ChapterHtmlSlimParser::endElement(&parser, "blockquote");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Following", 9);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_EQ(parser.currentTextBlock->size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->getWordStyleAt(2), EpdFontFamily::REGULAR);
  ChapterHtmlSlimParser::endElement(&parser, "td");
  EXPECT_EQ(parser.inlineStyleCount_, 0u);
  ChapterHtmlSlimParser::endElement(&parser, "tr");
  ChapterHtmlSlimParser::endElement(&parser, "table");
}

TEST_F(ChapterHtmlSlimParserTest, InheritsBodyTextIndentAndPreservesExplicitParagraphZero) {
  parser.cssParser->rulesBySelector_[".class-0"] =
      CssParser::parseInlineStyle("text-indent: 1.5em; text-align: justify");
  parser.cssParser->rulesBySelector_[".class_s4K-0"] = CssParser::parseInlineStyle("text-indent: 0");
  parser.cssParser->rulesBySelector_[".class_s4P-0"] = CssParser::parseInlineStyle("margin-top: 0; margin-bottom: 0");

  const XML_Char* bodyAttributes[] = {"class", "class-0", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle openingParagraph = parseParagraph("class_s4K-0");
  EXPECT_TRUE(openingParagraph.textIndentDefined);
  EXPECT_EQ(openingParagraph.textIndent, 0);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  const BlockStyle followingParagraph = parseParagraph("class_s4P-0");
  EXPECT_TRUE(followingParagraph.textIndentDefined);
  EXPECT_EQ(followingParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_F(ChapterHtmlSlimParserTest, HtmlIndentFlowsThroughBodyAndBodyIndentOverridesIt) {
  parser.cssParser->rulesBySelector_[".html-indent"] = CssParser::parseInlineStyle("text-indent: 1em");
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");

  const XML_Char* htmlAttributes[] = {"class", "html-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "html", htmlAttributes);
  EXPECT_EQ(parser.blockStyleBuf_[0].textIndent, 12);

  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle inheritedParagraph = parseParagraph("plain");
  EXPECT_TRUE(inheritedParagraph.textIndentDefined);
  EXPECT_EQ(inheritedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  ChapterHtmlSlimParser::endElement(&parser, "body");
  ChapterHtmlSlimParser::endElement(&parser, "html");
}

TEST_F(ChapterHtmlSlimParserTest, InheritsTextIndentFromDivAndKeepsParagraphOverride) {
  parser.cssParser->rulesBySelector_[".ancestor-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  parser.cssParser->rulesBySelector_[".zero"] = CssParser::parseInlineStyle("text-indent: 0");

  const XML_Char* divAttributes[] = {"class", "ancestor-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", divAttributes);

  const BlockStyle inheritedParagraph = parseParagraph("plain");
  EXPECT_TRUE(inheritedParagraph.textIndentDefined);
  EXPECT_EQ(inheritedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  const BlockStyle zeroParagraph = parseParagraph("zero");
  EXPECT_TRUE(zeroParagraph.textIndentDefined);
  EXPECT_EQ(zeroParagraph.textIndent, 0);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  ChapterHtmlSlimParser::endElement(&parser, "div");
}

TEST_F(ChapterHtmlSlimParserTest, BodyIndentRespectsSpacingAndForcedIndentSettings) {
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  parser.cssParser->rulesBySelector_[".zero"] = CssParser::parseInlineStyle("text-indent: 0");
  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  parser.extraParagraphSpacing = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(true, false);
  const BlockStyle spacedParagraph = parseParagraph("plain");
  EXPECT_EQ(spacedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  parser.extraParagraphSpacing = false;
  const BlockStyle unspacedParagraph = parseParagraph("plain");
  EXPECT_EQ(unspacedParagraph.textIndent, 18);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 18);

  parser.forceParagraphIndents = true;
  const BlockStyle forcedZeroParagraph = parseParagraph("zero");
  EXPECT_EQ(forcedZeroParagraph.textIndent, renderer.getFontAscenderSize(0));
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), renderer.getFontAscenderSize(0));

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

TEST_F(ChapterHtmlSlimParserTest, RootIndentIsIgnoredWhenEmbeddedStyleIsOff) {
  parser.embeddedStyle = false;
  parser.cssParser->rulesBySelector_[".body-indent"] = CssParser::parseInlineStyle("text-indent: 1.5em");
  parser.cssParser->rulesBySelector_[".plain"] = CssParser::parseInlineStyle("margin: 0");
  const XML_Char* bodyAttributes[] = {"class", "body-indent", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "body", bodyAttributes);

  const BlockStyle plainParagraph = parseParagraph("plain");
  EXPECT_FALSE(plainParagraph.textIndentDefined);
  EXPECT_EQ(parser.currentTextBlock->resolveFirstLineIndent(true, renderer, 0), 0);

  ChapterHtmlSlimParser::endElement(&parser, "body");
}

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

// <dfn>/<cite> carry the same browser-default italic styling as <i>/<em> --
// confirmed on real books that use <dfn> for foreign-language dialogue and
// <cite> for an attribution line, with no CSS backing at all (relying purely
// on the semantic tag's own default rendering).
TEST_F(ChapterHtmlSlimParserTest, DfnGetsDefaultItalicStyling) {
  ChapterHtmlSlimParser::startElement(&parser, "dfn", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Bonjour", 7);
  ChapterHtmlSlimParser::endElement(&parser, "dfn");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(EpdFontFamily::ITALIC), 0u);
}

TEST_F(ChapterHtmlSlimParserTest, CiteGetsDefaultItalicStyling) {
  ChapterHtmlSlimParser::startElement(&parser, "cite", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Seneca", 6);
  ChapterHtmlSlimParser::endElement(&parser, "cite");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(EpdFontFamily::ITALIC), 0u);
}

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

TEST_F(ChapterHtmlSlimParserTest, PreservesEmptyInlinePaddingBeforeDialogueText) {
  parser.cssParser->rulesBySelector_[".spacey"] = CssParser::parseInlineStyle("padding-left: 2em");
  ChapterHtmlSlimParser::characterData(&parser, "EERO:", 5);
  const XML_Char* attributes[] = {"class", "spacey", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, "Kappusiwai!", 11);
  parser.flushPartWordBuffer();

  ASSERT_EQ(parser.currentTextBlock->words.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "EERO:");
  EXPECT_EQ(parser.currentTextBlock->words[1], "Kappusiwai!");
  ASSERT_EQ(parser.currentTextBlock->inlinePaddings.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->inlinePaddings[0].wordIndex, 1u);
  EXPECT_EQ(parser.currentTextBlock->inlinePaddings[0].pixels, 24);
  EXPECT_TRUE(parser.currentTextBlock->wordContinues[1]);

  std::shared_ptr<TextBlock> renderedLine;
  ASSERT_TRUE(parser.currentTextBlock->layoutAndExtractLines(
      renderer, 0, 480,
      [&renderedLine](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) { renderedLine = std::move(line); }));
  ASSERT_NE(renderedLine, nullptr);
  ASSERT_EQ(renderedLine->wordCount(), 2u);
  EXPECT_EQ(renderedLine->wordXpos(0), 0);
  EXPECT_EQ(renderedLine->wordXpos(1), 24);
}

// SOF header of a 800 x 200 JPEG. The real streaming dimension parser is linked.
static const std::vector<uint8_t> jpegHeader = {0xff, 0xd8, 0xff, 0xc0, 0, 7, 8, 0, 200, 3, 32};

TEST_F(ChapterHtmlSlimParserTest, OrdinaryImageHeaderDoesNotLoanWhenHeapProbeSucceeds) {
  epub.probeBytes = jpegHeader;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 1u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, RetriesImageHeaderWithLoanAndKeepsLazySource) {
  epub.probeBytes = jpegHeader;
  epub.requireLoan = true;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 2u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 1u);
  EXPECT_TRUE(renderer.hasFrameBuffer());
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 120);
}

TEST_F(ChapterHtmlSlimParserTest, FailedOrMalformedProbeLoansExtractionAndReturnsBuffer) {
  for (const bool streamFails : {false, true}) {
    epub.probeBytes = streamFails ? jpegHeader : std::vector<uint8_t>{0xff, 0xd8, 0x00};
    epub.streamFails = streamFails;
    epub.streamReadCount = epub.extractCount = 0;
    renderer.loans = 0;
    const XML_Char* attributes[] = {"src", "broken.jpg", nullptr};
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    EXPECT_EQ(epub.streamReadCount, 2u);
    EXPECT_EQ(epub.extractCount, 1u);
    EXPECT_TRUE(epub.extractHadLoan);
    EXPECT_EQ(renderer.loans, 2u);
    EXPECT_TRUE(renderer.hasFrameBuffer());
    EXPECT_TRUE(!parser.currentPage || parser.currentPage->elements.empty());
    ChapterHtmlSlimParser::endElement(&parser, "img");
  }
}

TEST_F(ChapterHtmlSlimParserTest, FullFileFallbackReturnsLoanBeforeDecoder) {
  epub.probeBytes = {0xff, 0xd8, 0};
  epub.extractSucceeds = true;
  const XML_Char* attributes[] = {"src", "unusual.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.extractCount, 1u);
  EXPECT_TRUE(epub.extractHadLoan);
  EXPECT_FALSE(GfxRenderer::fileProbeHadLoan);
  EXPECT_TRUE(renderer.hasFrameBuffer());
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_EQ(parser.currentPage->elements.size(), 1u);
}

TEST_F(ChapterHtmlSlimParserTest, ImageAdmissionStillRejectsBeforeAnyProbeOrLoan) {
  MemoryBudget::imageAllowed = false;
  epub.probeBytes = jpegHeader;
  epub.requireLoan = true;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(epub.extractCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  EXPECT_TRUE(parser.lowMemoryImageFallback);
}

TEST_F(ChapterHtmlSlimParserTest, NestedProbeLoanDoesNotReturnOuterStorage) {
  epub.probeBytes = {0xff, 0xd8, 0};
  const XML_Char* attributes[] = {"src", "broken.jpg", nullptr};
  {
    GfxRenderer::FrameBufferLoan outer(renderer);
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    EXPECT_FALSE(renderer.hasFrameBuffer());
    EXPECT_EQ(renderer.loans, 1u);
  }
  EXPECT_TRUE(renderer.hasFrameBuffer());
}

TEST_F(ChapterHtmlSlimParserTest, UsesOptimizerImageDimensionsWithoutReadingTheCompressedImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 800;
  epub.optimizerImageHeight = 7;
  const XML_Char* attributes[] = {"src", "wide.jpg", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  EXPECT_EQ(epub.streamReadCount, 0u);
  EXPECT_EQ(renderer.loans, 0u);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageImage);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 480);
  EXPECT_EQ(image.getHeight(), 4);
  EXPECT_FALSE(static_cast<const PageImage&>(*parser.currentPage->elements.front()).isInlineImage());
}

TEST_F(ChapterHtmlSlimParserTest, PlacesSmallImageInsideTextLine) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "Before", 6);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "After", 5);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  const auto& line = static_cast<const PageLine&>(*parser.currentPage->elements[0]);
  EXPECT_EQ(line.getBlock()->wordCount(), 2u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements[1]);
  EXPECT_TRUE(image.isInlineImage());
  EXPECT_EQ(image.yPos, parser.currentPage->elements[0]->yPos);
  EXPECT_EQ(image.getImageBlock().getWidth(), 16);
  EXPECT_TRUE(parser.pendingInlineImages.empty());
}

TEST_F(ChapterHtmlSlimParserTest, WrapsTextAfterInlineImageWithoutSplittingTheImage) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 22;
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  ChapterHtmlSlimParser::characterData(&parser, "B", 1);
  parser.flushPartWordBuffer();
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[2]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->xPos, 4);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, parser.currentPage->elements[1]->yPos);
  EXPECT_EQ(parser.currentPage->elements[2]->yPos, 16);
}

TEST_F(ChapterHtmlSlimParserTest, AlignsTextWithTallerInlineImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 20;
  epub.optimizerImageHeight = 32;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 16);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 0);
  EXPECT_EQ(parser.currentPageNextY, 32);
}

TEST_F(ChapterHtmlSlimParserTest, HonorsExplicitBlockDisplayForSmallImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 16;
  epub.optimizerImageHeight = 16;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "display: block", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_GE(parser.currentPage->elements[1]->yPos, 16);
}

TEST_F(ChapterHtmlSlimParserTest, HonorsExplicitInlineDisplayForTallerImage) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 20;
  epub.optimizerImageHeight = 48;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "display: inline", nullptr};

  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
  ChapterHtmlSlimParser::endElement(&parser, "img");
  parser.makePages();

  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  EXPECT_EQ(parser.currentPage->elements[0]->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.currentPage->elements[1]->getTag(), TAG_PageImage);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 32);
  EXPECT_EQ(parser.currentPage->elements[1]->yPos, 0);
  EXPECT_EQ(parser.currentPageNextY, 48);
}

TEST_F(ChapterHtmlSlimParserTest, BoundsPendingImagesInAnIconOnlyParagraph) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 1;
  epub.optimizerImageHeight = 1;
  const XML_Char* attributes[] = {"src", "icon.jpg", nullptr};

  for (int i = 0; i < 40; ++i) {
    ChapterHtmlSlimParser::startElement(&parser, "img", attributes);
    ChapterHtmlSlimParser::endElement(&parser, "img");
    EXPECT_LT(parser.pendingInlineImages.size(), parser.MAX_PENDING_INLINE_IMAGES);
  }
  parser.makePages();

  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_TRUE(parser.pendingInlineImages.empty());
  EXPECT_EQ(parser.currentPage->elements.size(), 40u);
}

TEST_F(ChapterHtmlSlimParserTest, HtmlWidthAttributeSizesImageWhenNoCssApplies) {
  // Some authors size a decorative inline image purely via the legacy HTML
  // width=/height= attributes, with no CSS at all -- confirmed on a real book
  // whose embedded Tumblr image was downloaded at its full 1200x1600
  // resolution, with width="150" (height omitted) expressing the intended
  // small display size. Previously ignored entirely, so the image rendered
  // scaled to fill the viewport instead (roughly 480x640: ~10x the area).
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 1200;
  epub.optimizerImageHeight = 1600;
  const XML_Char* attributes[] = {"src", "photo.jpg", "width", "150", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  ASSERT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageImage);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 150);
  EXPECT_EQ(image.getHeight(), 200);
}

TEST_F(ChapterHtmlSlimParserTest, HtmlHeightAttributeSizesImageWhenNoCssApplies) {
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 1200;
  epub.optimizerImageHeight = 1600;
  const XML_Char* attributes[] = {"src", "photo.jpg", "height", "200", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 150);
  EXPECT_EQ(image.getHeight(), 200);
}

TEST_F(ChapterHtmlSlimParserTest, CssImageWidthOverridesHtmlWidthAttribute) {
  // Real CSS/inline style must win over the legacy attribute, matching the
  // established align="" fallback precedent -- the attribute only fills a
  // gap, it never competes with an actual style rule.
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 1200;
  epub.optimizerImageHeight = 1600;
  const XML_Char* attributes[] = {"src", "photo.jpg", "width", "150", "style", "width: 60px", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 60);
}

TEST_F(ChapterHtmlSlimParserTest, CssPercentImageWidthDoesNotShrinkBelowNativeSize) {
  // Publishers wrap figures in boxes such as `width: 60%`, sized assuming a
  // much wider screen than ours -- 60% of a tablet's width still leaves a
  // picture close to its native size there, but the same rule shrinks a
  // diagram to a thumbnail on our narrower page. A percentage should still be
  // able to *enlarge* a picture, just never shrink it below what it actually
  // is. Native 300x200; 60% of the 480px viewport is 288, below native --
  // expect the floor to hold it at its native 300x200 instead.
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 300;
  epub.optimizerImageHeight = 200;
  const XML_Char* attributes[] = {"src", "diagram.jpg", "style", "width: 60%", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 300);
  EXPECT_EQ(image.getHeight(), 200);
}

TEST_F(ChapterHtmlSlimParserTest, CssPercentImageWidthCanStillEnlargeAndIsCappedByContainer) {
  // The floor only raises a percentage width up to native size -- a
  // percentage that already resolves above native size still enlarges the
  // picture as requested (existing behavior, unaffected by the floor), and
  // one that would exceed the container is still capped to the container
  // (also existing behavior). Native 100x100; 90% of 480 is 432 (well above
  // native) -> unaffected by the floor, capped by nothing here.
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 100;
  epub.optimizerImageHeight = 100;
  const XML_Char* attributes[] = {"src", "icon.jpg", "style", "width: 90%", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 432);
  EXPECT_EQ(image.getHeight(), 432);
}

TEST_F(ChapterHtmlSlimParserTest, CssPixelImageWidthStillShrinksBelowNativeSize) {
  // The floor is specific to percentage units -- a publisher who explicitly
  // picked a pixel width smaller than native (e.g. `width: 60px`) made a
  // deliberate sizing choice, not a viewport-relative accident, and that
  // choice must still be honored.
  epub.optimizerImageAvailable = true;
  epub.optimizerImageWidth = 300;
  epub.optimizerImageHeight = 200;
  const XML_Char* attributes[] = {"src", "diagram.jpg", "style", "width: 60px", nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "img", attributes);

  ASSERT_NE(parser.currentPage, nullptr);
  const auto& image = static_cast<const PageImage&>(*parser.currentPage->elements.front()).getImageBlock();
  EXPECT_EQ(image.getWidth(), 60);
  EXPECT_EQ(image.getHeight(), 40);
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

TEST_F(ChapterHtmlSlimParserTest, StablePageOffsetsCollapseClusteredWhitespace) {
  parser.trackReferenceCharacters = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);

  constexpr char text[] = "  Alpha     Beta ";
  ChapterHtmlSlimParser::characterData(&parser, text, sizeof(text) - 1);
  parser.flushPartWordBuffer();

  ASSERT_EQ(parser.currentTextBlock->wordReferenceOffsets.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[1], 6u);
  EXPECT_EQ(parser.referenceTextOffset, 10u);
  EXPECT_TRUE(parser.referenceWhitespacePending);
}

TEST_F(ChapterHtmlSlimParserTest, StablePageOffsetsResumeAfterNestedExcludedMarkup) {
  parser.trackReferenceCharacters = true;
  parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);

  ChapterHtmlSlimParser::startElement(&parser, "html", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "head", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "style", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "p { display: block; }", 21);
  ChapterHtmlSlimParser::endElement(&parser, "style");
  ChapterHtmlSlimParser::endElement(&parser, "head");
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "svg", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "metadata", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "not book text", 13);
  ChapterHtmlSlimParser::endElement(&parser, "metadata");
  ChapterHtmlSlimParser::endElement(&parser, "svg");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Visible text ", 13);
  parser.flushPartWordBuffer();

  EXPECT_EQ(parser.referenceExcludedUntilDepth, INT_MAX);
  EXPECT_EQ(parser.referenceTextOffset, 12u);
  ASSERT_EQ(parser.currentTextBlock->wordReferenceOffsets.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[0], 0u);
  EXPECT_EQ(parser.currentTextBlock->wordReferenceOffsets[1], 8u);
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
  parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
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

TEST_F(ChapterHtmlSlimParserTest, NumbersOrderedListsAndRestartsNestedCounters) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");

  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ol");

  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
}

TEST_F(ChapterHtmlSlimParserTest, HonorsOrderedListStartAndItemValue) {
  const XML_Char* listAttributes[] = {"start", "5", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ol", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "5.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  const XML_Char* itemAttributes[] = {"value", "9", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "li", itemAttributes);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "9.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "10.");
}

TEST_F(ChapterHtmlSlimParserTest, SupportsNegativeOrderedListValues) {
  const XML_Char* listAttributes[] = {"start", "-2", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ol", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "-2.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "-1.");
}

TEST_F(ChapterHtmlSlimParserTest, SupportsMarkerFreeListsAndContainerInsets) {
  const XML_Char* listAttributes[] = {"style", "list-style-type: none; margin-left: 10px; padding-left: 5px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ul", listAttributes);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);

  EXPECT_TRUE(parser.currentTextBlock->isEmpty());
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().leftInset(), 15);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenNestedListDoesNotResetOuterCounter) {
  ChapterHtmlSlimParser::startElement(&parser, "ol", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  EXPECT_EQ(parser.currentTextBlock->words[0], "1.");
  ChapterHtmlSlimParser::endElement(&parser, "li");

  const XML_Char* hidden[] = {"hidden", "", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "ul", hidden);
  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "li");
  ChapterHtmlSlimParser::endElement(&parser, "ul");

  ChapterHtmlSlimParser::startElement(&parser, "li", nullptr);
  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "2.");
}

}  // namespace

namespace {
TEST_F(ChapterHtmlSlimParserTest, ScalableHeadingLevelsAndBodySizeCeiling) {
  renderer.scalableBaseSize = 12;
  const XML_Char* attrs[] = {nullptr};
  const uint8_t expected[] = {24, 18, 14, 12, 10, 8};
  for (int level = 1; level <= 6; ++level) {
    const char tag[] = {'h', static_cast<char>('0' + level), 0};
    ChapterHtmlSlimParser::startElement(&parser, tag, attrs);
    EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, expected[level - 1]);
    ChapterHtmlSlimParser::endElement(&parser, tag);
  }
  renderer.scalableBaseSize = 22;
  ChapterHtmlSlimParser::startElement(&parser, "h1", attrs);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 44);
}

TEST_F(ChapterHtmlSlimParserTest, BlockSizesInheritAndRestoreAcrossSiblings) {
  renderer.scalableBaseSize = 12;
  const XML_Char* parent[] = {"style", "font-size: 150%", nullptr};
  const XML_Char* child[] = {"style", "font-size: 0.5em", nullptr};
  const XML_Char* plain[] = {nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", parent);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::startElement(&parser, "p", child);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 9);
  ChapterHtmlSlimParser::characterData(&parser, "Small", 5);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::characterData(&parser, "Parent", 6);
  ChapterHtmlSlimParser::startElement(&parser, "p", plain);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ChapterHtmlSlimParser::startElement(&parser, "p", plain);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 12);
}

TEST_F(ChapterHtmlSlimParserTest, TextAfterTableKeepsParentFontSize) {
  renderer.scalableBaseSize = 12;
  const XML_Char* parent[] = {"style", "font-size: 150%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", parent);
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "table");
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontScale, 384);
  ChapterHtmlSlimParser::characterData(&parser, "Parent", 6);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_FALSE(parser.currentPage->elements.empty());
  const auto& line = static_cast<const PageLine&>(*parser.currentPage->elements.back());
  EXPECT_EQ(line.getBlock()->getBlockStyle().fontSize, 18);
  EXPECT_EQ(line.getBlock()->getBlockStyle().lineHeight, 36);
}

TEST_F(ChapterHtmlSlimParserTest, RootRelativeSizesAndAbsoluteSizesRespectReaderZoom) {
  renderer.scalableBaseSize = 16;
  const XML_Char* html[] = {"style", "font-size: 125%", nullptr};
  const XML_Char* body[] = {"style", "font-size: 150%", nullptr};
  const XML_Char* rem[] = {"style", "font-size: 1rem", nullptr};
  const XML_Char* points[] = {"style", "font-size: 12pt", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "html", html);
  ChapterHtmlSlimParser::startElement(&parser, "body", body);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 30);
  ChapterHtmlSlimParser::startElement(&parser, "p", rem);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 20);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::startElement(&parser, "p", points);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 16);
}

TEST_F(ChapterHtmlSlimParserTest, PublisherSizeOverridesHeadingDefaultAndInlineSizesStayUniform) {
  renderer.scalableBaseSize = 12;
  const XML_Char* heading[] = {"style", "font-size: 150%", nullptr};
  const XML_Char* span[] = {"style", "font-size: 300%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
  ChapterHtmlSlimParser::startElement(&parser, "span", span);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
}

TEST_F(ChapterHtmlSlimParserTest, BitmapAndLightModesKeepUniformSize) {
  const XML_Char* heading[] = {"style", "font-size: 200%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 0);
  ChapterHtmlSlimParser::endElement(&parser, "h1");
  renderer.scalableBaseSize = 12;
  parser.renderMode = EpubRenderMode::Light;
  ChapterHtmlSlimParser::startElement(&parser, "h1", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 0);
}

TEST_F(ChapterHtmlSlimParserTest, DisabledPublisherStylingKeepsSemanticHeadings) {
  renderer.scalableBaseSize = 12;
  parser.embeddedStyle = false;
  const XML_Char* heading[] = {"style", "font-size: 300%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h2", heading);
  EXPECT_EQ(parser.currentTextBlock->getBlockStyle().fontSize, 18);
}

TEST_F(ChapterHtmlSlimParserTest, LargerHeadingsWrapAndReserveTheirActualHeight) {
  renderer.scalableBaseSize = 12;
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 100;
  const XML_Char* attrs[] = {nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", attrs);
  const char* text = "four four four four four four four four";
  ChapterHtmlSlimParser::characterData(&parser, text, std::strlen(text));
  ChapterHtmlSlimParser::endElement(&parser, "h1");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_GT(parser.currentPage->elements.size(), 2u);
  int previousY = -48;
  for (const auto& element : parser.currentPage->elements) {
    ASSERT_EQ(element->getTag(), TAG_PageLine);
    const auto& line = static_cast<const PageLine&>(*element);
    EXPECT_EQ(line.getBlock()->getBlockStyle().fontSize, 24);
    EXPECT_EQ(line.getBlock()->getBlockStyle().lineHeight, 48);
    EXPECT_GE(line.yPos - previousY, 48);
    previousY = line.yPos;
  }
}

TEST(CssFontSizeTest, StrictValuesAndCascade) {
  for (const char* value : {"12badpx", "12foorem", "10vw", "12", "nanem", "-2em", "0px"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(std::string("font-size: ") + value).hasFontSize()) << value;
  }
  for (const char* value : {"150%", "1.25em", "1rem", "12pt", "16px", "larger", "small", "inherit"}) {
    EXPECT_TRUE(CssParser::parseInlineStyle(std::string("font-size: ") + value).hasFontSize()) << value;
  }
  const auto style = CssParser::parseInlineStyle("font-size: 150%; font-size: nonsense");
  EXPECT_TRUE(style.hasFontSize());
  EXPECT_FLOAT_EQ(style.fontSize.value, 150);
}
}  // namespace

TEST(CssBorderTest, SuppressionAndDeclarationOrder) {
  for (const char* declarations :
       {"border: none", "border: HIDDEN !important", "border: 0", "border: solid 0px black", "border-style: none",
        "border-width: 0rem", "border-width: 0 0px 0em 0pt", "border-style: none hidden",
        "border: solid; border-style: none", "border-width: 0; border-style: solid",
        "border: none; border-style: solid; border-width: 0",
        "border-top: none; border-right: 0; border-bottom: hidden; border-left-width: 0"}) {
    SCOPED_TRACE(declarations);
    const auto style = CssParser::parseInlineStyle(declarations);
    EXPECT_TRUE(style.defined.border);
    EXPECT_TRUE(style.suppressesHorizontalRule());
  }
  for (const char* declarations :
       {"", "border: solid", "border-style: dashed", "border-width: thin", "border-top: none",
        "border: none; border: 1px solid black", "border: none; border-top-style: solid", "border-width: 0 1px",
        "border: 0; border-width: 2px; border-style: solid", "border-style: none; border-style: solid"}) {
    EXPECT_FALSE(CssParser::parseInlineStyle(declarations).suppressesHorizontalRule()) << declarations;
  }
}

TEST(CssBorderTest, InvalidLonghandsDoNotOverrideSuppression) {
  for (const char* value : {"-1px", "1badpx", "nanpx", "2%", "2", "nonsense", "1px 1px 1px 1px 1px"}) {
    const auto style = CssParser::parseInlineStyle(std::string("border-width: 0; border-width: ") + value);
    EXPECT_TRUE(style.suppressesHorizontalRule()) << value;
  }
  EXPECT_TRUE(CssParser::parseInlineStyle("border-style: none; border-style: nonsense").suppressesHorizontalRule());
}

TEST(CssBorderTest, CascadeRestoresEdgesWithoutLosingZeroWidths) {
  auto style = CssParser::parseInlineStyle("border: none");
  style.applyOver(CssParser::parseInlineStyle("border-top-style: solid"));
  EXPECT_FALSE(style.suppressesHorizontalRule());
  style.applyOver(CssParser::parseInlineStyle("border-top-width: 0"));
  EXPECT_TRUE(style.suppressesHorizontalRule());
  style.applyOver(CssParser::parseInlineStyle("border-top-width: medium"));
  EXPECT_FALSE(style.suppressesHorizontalRule());
  style.reset();
  EXPECT_FALSE(style.defined.border);
  EXPECT_FALSE(style.suppressesHorizontalRule());
}

TEST_F(ChapterHtmlSlimParserTest, CackleTransitionKeepsOnlyPublisherOrnament) {
  cssParser.rulesBySelector_["hr.transition"] = CssParser::parseInlineStyle("display: block; border: none; margin: 0");
  cssParser.rulesBySelector_["div.ornament"] = CssParser::parseInlineStyle("text-align: center; margin: 0");
  const XML_Char* hrAttrs[] = {"class", "transition", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", hrAttrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  EXPECT_EQ(parser.currentPageNextY, 0);
  const XML_Char* ornamentAttrs[] = {"class", "ornament", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", ornamentAttrs);
  ChapterHtmlSlimParser::characterData(&parser, "—", 3);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  // Starting the following paragraph seals the ornament's text block.
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageLine);
  EXPECT_EQ(parser.depth, 1);
}

TEST_F(ChapterHtmlSlimParserTest, SuppressedRuleRetainsExplicitSpacing) {
  const XML_Char* attrs[] = {"style", "border: none; margin: 7px 0 9px; padding: 2px 0 3px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", attrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  EXPECT_TRUE(parser.currentPage->elements.empty());
  EXPECT_EQ(parser.currentPageNextY, 21);
}

TEST_F(ChapterHtmlSlimParserTest, PlainAndExplicitlyVisibleRulesStillRender) {
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  const XML_Char* attrs[] = {"style", "border: none; border-top: 1px solid black", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "hr", attrs);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 2u);
  for (const auto& element : parser.currentPage->elements) EXPECT_EQ(element->getTag(), TAG_PageHorizontalRule);
}

TEST_F(ChapterHtmlSlimParserTest, ParentBorderDoesNotHideChildRule) {
  const XML_Char* attrs[] = {"style", "border: none", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "div", attrs);
  ChapterHtmlSlimParser::startElement(&parser, "hr", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "hr");
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->getTag(), TAG_PageHorizontalRule);
}

TEST_F(ChapterHtmlSlimParserTest, SoftFlushAppliesTopSpacingOnFirstEmittedLineOnly) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 40;
  BlockStyle style;
  style.marginTop = 7;
  style.paddingTop = 5;
  style.marginBottom = 9;
  style.paddingBottom = 3;
  style.textIndent = 8;
  style.textIndentDefined = true;
  parser.currentTextBlock->setBlockStyle(style);
  parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true);
  EXPECT_EQ(parser.wordsExtractedInBlock, 0);
  EXPECT_EQ(parser.currentPageNextY, 0);
  EXPECT_FALSE(parser.currentTextBlock->isContinuation());
  for (int i = 0; i < 8; ++i) parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true);
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_GT(parser.currentPage->elements.size(), 0u);
  EXPECT_EQ(parser.currentPage->elements.front()->yPos, 12);
  const int emitted = parser.currentPage->elements.size();
  EXPECT_EQ(parser.currentPageNextY, 12 + emitted * 16);
  EXPECT_TRUE(parser.currentTextBlock->isContinuation());
  parser.makePages();
  EXPECT_EQ(parser.currentPageNextY, 24 + int(parser.currentPage->elements.size()) * 16);
  for (size_t i = 0; i < parser.currentPage->elements.size(); ++i) {
    EXPECT_EQ(parser.currentPage->elements[i]->yPos, 12 + int(i) * 16);
  }
}

TEST_F(ChapterHtmlSlimParserTest, EmptyFinalFlushDoesNotConsumeTopSpacing) {
  BlockStyle style;
  style.marginTop = 7;
  style.paddingTop = 5;
  parser.currentTextBlock->setBlockStyle(style);
  parser.makePages();
  EXPECT_EQ(parser.currentPageNextY, 0);
  parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements.front()->yPos, 12);
}

TEST_F(ChapterHtmlSlimParserTest, FullyFlushedParagraphResetsSpacingForReusedBlock) {
  BlockStyle style;
  style.marginTop = 7;
  style.paddingTop = 5;
  parser.currentTextBlock->setBlockStyle(style);
  parser.currentTextBlock->addWord("one", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true, true);
  EXPECT_EQ(parser.currentPageNextY, 28);
  parser.currentTextBlock->addWord("two", EpdFontFamily::REGULAR);
  parser.flushLongTextRunIfNeeded(true, true);
  EXPECT_EQ(parser.currentPageNextY, 44);
  parser.startNewTextBlock(style);
  parser.currentTextBlock->addWord("three", EpdFontFamily::REGULAR);
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 3u);
  EXPECT_EQ(parser.currentPage->elements.back()->yPos, 56);
}

TEST_F(ChapterHtmlSlimParserTest, FragmentAppendChecksBoundBeforeCallbackEnds) {
  renderer.textAdvancePerChar = 4;
  for (size_t i = 0; i < parser.bufferedWordsBeforeLayoutLimit() + 1; ++i) {
    std::strcpy(parser.partWordBuffer, "word");
    parser.partWordBufferIndex = 4;
    parser.flushPartWordBuffer();
  }
  EXPECT_GT(parser.wordsExtractedInBlock, 0);
  EXPECT_LE(parser.currentTextBlock->size(), parser.bufferedWordsBeforeLayoutLimit());
}

TEST_F(ChapterHtmlSlimParserTest, HugeCjkCallbackDoesNotRetainWholeRunCapacity) {
  renderer.textAdvancePerChar = 4;
  std::string text;
  for (int i = 0; i < 6000; ++i) text += "\xE4\xB8\xAD";
  parser.completePageFn = [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) {};
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  parser.flushPartWordBuffer();
  EXPECT_FALSE(parser.lowMemoryAbort);
  EXPECT_GT(parser.wordsExtractedInBlock, 5000);
  EXPECT_LT(parser.currentTextBlock->wordStyles.capacity(), 1024u);
  EXPECT_EQ(parser.visibleTextOffset, 6000u);
  EXPECT_EQ(parser.wordsExtractedInBlock + parser.currentTextBlock->size(), 6000u);
  if (!parser.currentTextBlock->isEmpty()) {
    EXPECT_EQ(parser.currentTextBlock->visibleOffsetAt(0), uint32_t(parser.wordsExtractedInBlock));
  }
}

TEST_F(ChapterHtmlSlimParserTest, FragmentFlushKeepsRubyGroupBuffered) {
  renderer.textAdvancePerChar = 4;
  parser.inRuby = true;
  for (size_t i = 0; i < parser.bufferedWordsBeforeLayoutLimit() + 1; ++i) {
    std::strcpy(parser.partWordBuffer, "word");
    parser.partWordBufferIndex = 4;
    parser.flushPartWordBuffer();
  }
  EXPECT_EQ(parser.wordsExtractedInBlock, 0);
  EXPECT_EQ(parser.currentTextBlock->size(), parser.bufferedWordsBeforeLayoutLimit() + 1);
  parser.inRuby = false;
  parser.flushLongTextRunIfNeeded();
  EXPECT_GT(parser.wordsExtractedInBlock, 0);
}

TEST_F(ChapterHtmlSlimParserTest, FragmentFlushKeepsBufferedTableCellIntact) {
  renderer.textAdvancePerChar = 4;
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "td", nullptr);
  ASSERT_NE(parser.currentTableBuffer, nullptr);
  for (size_t i = 0; i < parser.bufferedWordsBeforeLayoutLimit() + 1; ++i) {
    std::strcpy(parser.partWordBuffer, "word");
    parser.partWordBufferIndex = 4;
    parser.flushPartWordBuffer();
  }
  EXPECT_EQ(parser.wordsExtractedInBlock, 0);
  EXPECT_EQ(parser.currentTextBlock->size(), parser.bufferedWordsBeforeLayoutLimit() + 1);
}

TEST_F(ChapterHtmlSlimParserTest, SingleHugeCallbackHonorsPreviewPageLimit) {
  renderer.textAdvancePerChar = 4;
  parser.viewportHeight = 48;
  parser.previewAnchor = "note";
  parser.previewMaxPages = 1;
  parser.previewAnchorFound = true;
  int pages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) { ++pages; };
  std::string text;
  for (int i = 0; i < 2000; ++i) text += "word ";
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  EXPECT_TRUE(parser.previewStopRequested);
  EXPECT_EQ(pages, 1);
  EXPECT_EQ(parser.completedPageCount, 1);
}

TEST_F(ChapterHtmlSlimParserTest, SoftFlushLeavesTrailingFootnotesForFinalization) {
  renderer.textAdvancePerChar = 4;
  parser.viewportWidth = 40;
  for (int i = 0; i < 8; ++i) parser.currentTextBlock->addWord("word", EpdFontFamily::REGULAR);
  FootnoteEntry note{};
  std::strcpy(note.number, "1");
  std::strcpy(note.href, "#note");
  note.linkId = 1;
  parser.pendingFootnotes.push_back({8, note});
  parser.flushLongTextRunIfNeeded(true);
  EXPECT_EQ(parser.pendingFootnotes.size(), 1u);
  parser.makePages();
  EXPECT_TRUE(parser.pendingFootnotes.empty());
  ASSERT_EQ(parser.currentPage->footnotes.size(), 1u);
  EXPECT_STREQ(parser.currentPage->footnotes[0].href, "#note");
}

TEST_F(ChapterHtmlSlimParserTest, FragmentFlushKeepsActiveLinkOnEveryEmittedPage) {
  renderer.textAdvancePerChar = 4;
  parser.viewportHeight = 64;
  int linkedPages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
    ASSERT_EQ(page->footnotes.size(), 1u);
    EXPECT_STREQ(page->footnotes[0].href, "#note");
    ++linkedPages;
  };
  const XML_Char* attrs[] = {"href", "#note", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "a", attrs);
  std::string text;
  for (int i = 0; i < 1000; ++i) text += "linked ";
  ChapterHtmlSlimParser::characterData(&parser, text.data(), text.size());
  EXPECT_GT(linkedPages, 0);
  ChapterHtmlSlimParser::endElement(&parser, "a");
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  ASSERT_EQ(parser.currentPage->footnotes.size(), 1u);
  EXPECT_STREQ(parser.currentPage->footnotes[0].href, "#note");
}

TEST_F(ChapterHtmlSlimParserTest, PageBreakBeforeDoesNotRepeatPreviousBottomSpacing) {
  BlockStyle previous;
  previous.marginBottom = 11;
  previous.paddingBottom = 7;
  parser.currentTextBlock->setBlockStyle(previous);
  parser.extraParagraphSpacing = true;
  parser.currentTextBlock->addWord("previous", EpdFontFamily::REGULAR);
  int pages = 0;
  parser.completePageFn = [&](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) { ++pages; };
  const XML_Char* attrs[] = {"style", "page-break-before: always", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "next", 4);
  parser.flushPartWordBuffer();
  parser.makePages();
  EXPECT_EQ(pages, 1);
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  EXPECT_EQ(parser.currentPage->elements[0]->yPos, 0);
}

TEST_F(ChapterHtmlSlimParserTest, CallbackChunkingPreservesVisibleAndReferencePageOffsets) {
  renderer.textAdvancePerChar = 4;
  const std::string fragment = "word \xE4\xB8\xAD\xF0\x9F\x98\x80 text ";
  std::string text;
  for (int i = 0; i < 600; ++i) text += fragment;
  auto layout = [&](size_t chunkSize) {
    parser.currentTextBlock = std::make_unique<ParsedText>(false, false, false, false, false, 0, BlockStyle{}, true);
    parser.trackReferenceCharacters = true;
    parser.currentPage.reset();
    parser.currentPageNextY = 0;
    parser.completedPageCount = 0;
    parser.wordsExtractedInBlock = 0;
    parser.visibleTextOffset = 0;
    parser.referenceTextOffset = 0;
    parser.referenceTextStarted = false;
    parser.referenceWhitespacePending = false;
    parser.currentTextRunBytes = 0;
    parser.partWordBufferIndex = 0;
    parser.nextWordContinues = false;
    std::vector<std::array<uint32_t, 3>> pages;
    parser.completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t visible, uint32_t reference) {
      pages.push_back({visible, reference, uint32_t(page->elements.size())});
    };
    for (size_t offset = 0; offset < text.size(); offset += chunkSize) {
      ChapterHtmlSlimParser::characterData(&parser, text.data() + offset, std::min(chunkSize, text.size() - offset));
    }
    parser.flushPartWordBuffer();
    parser.makePages();
    if (parser.currentPage && !parser.currentPage->elements.empty()) parser.completeCurrentPage();
    EXPECT_FALSE(parser.lowMemoryAbort);
    return pages;
  };
  const auto oneCallback = layout(text.size());
  const auto splitCallbacks = layout(fragment.size());
  EXPECT_GT(oneCallback.size(), 1u);
  EXPECT_EQ(oneCallback, splitCallbacks);
}

TEST_F(ChapterHtmlSlimParserTest, ContextSelectorsMatchIdsClassesAndDirectChildren) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("section.chapter > p.note.wide",
                                                  CssParser::parseInlineStyle("font-style: italic")));
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("#intro", CssParser::parseInlineStyle("font-weight: bold")));
  std::vector<CssAncestorEntry> ancestors{{0, "section", "chapter", ""}};
  auto style = cssParser.resolveStyle("p", "wide note", ancestors, "intro");
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
  ancestors.push_back({1, "div", "", ""});
  EXPECT_FALSE(cssParser.resolveStyle("p", "wide note", ancestors).hasFontStyle());
}

TEST_F(ChapterHtmlSlimParserTest, FirstLetterRulesDoNotStyleWholeParagraph) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("p.opening::first-letter",
                                                  CssParser::parseInlineStyle("initial-letter: 3; font-weight: bold")));
  EXPECT_FALSE(cssParser.resolveStyle("p", "opening").hasFontWeight());
  const auto first = cssParser.resolveStyle("p", "opening", {}, {}, true);
  EXPECT_EQ(first.initialLetter, 3);
  const XML_Char* attrs[] = {"class", "opening", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Hello world", 11);
  parser.flushPartWordBuffer();
  EXPECT_STREQ(parser.dropCap.text, "H");
  ASSERT_EQ(parser.currentTextBlock->words.size(), 2u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "ello");
  parser.makePages();
  ASSERT_TRUE(parser.currentPage);
  auto cap = std::find_if(parser.currentPage->elements.begin(), parser.currentPage->elements.end(),
                          [](const auto& item) { return item->getTag() == TAG_PageDropCap; });
  ASSERT_NE(cap, parser.currentPage->elements.end());
  EXPECT_STREQ(static_cast<PageDropCap*>(cap->get())->getText(), "H");
  EXPECT_EQ(parser.visibleTextOffset, 11u);
}

TEST_F(ChapterHtmlSlimParserTest, OversizedDropCapSpanFallsBackWithoutLosingLetters) {
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const XML_Char* attrs[] = {"style", "float: left; font-size: 3em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Hello", 5);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  parser.flushPartWordBuffer();
  EXPECT_EQ(parser.dropCap.length, 0);
  ASSERT_EQ(parser.currentTextBlock->words.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "Hello");
}

TEST_F(ChapterHtmlSlimParserTest, PreservedWhitespaceKeepsSpacesAndLineBreaks) {
  const XML_Char* attrs[] = {"style", "white-space: pre-wrap", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "A  B", 4);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->words.size(), 4u);
  EXPECT_EQ(parser.currentTextBlock->words[1], " ");
  EXPECT_EQ(parser.currentTextBlock->words[2], " ");
  EXPECT_TRUE(parser.currentTextBlock->wordNoSpaceBefore.back());
  ChapterHtmlSlimParser::characterData(&parser, "\nC", 2);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->words.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "C");
  EXPECT_EQ(parser.visibleTextOffset, 6u);
}

TEST_F(ChapterHtmlSlimParserTest, WhitespaceOverrideRestoresParentStyle) {
  const XML_Char* pre[] = {"style", "white-space: pre-wrap", nullptr};
  const XML_Char* normal[] = {"style", "white-space: normal", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", pre);
  EXPECT_TRUE(parser.effectivePreserveWhitespace);
  ChapterHtmlSlimParser::startElement(&parser, "span", normal);
  EXPECT_FALSE(parser.effectivePreserveWhitespace);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  EXPECT_TRUE(parser.effectivePreserveWhitespace);
}

TEST_F(ChapterHtmlSlimParserTest, BorderLonghandsPreserveOtherSidesAndWidth) {
  auto style = CssParser::parseInlineStyle("border: 2px dashed; border-left-style: none; border-top-width: 4px");
  EXPECT_TRUE(style.hasVisibleBorder());
  EXPECT_EQ(style.borders[0].width, 4);
  EXPECT_EQ(style.borders[1].style, CssBorderStyle::Dashed);
  EXPECT_FALSE(style.borders[3].visible());
  style.applyOver(CssParser::parseInlineStyle("border-left-style: solid"));
  EXPECT_EQ(style.borders[3].width, 2);
  EXPECT_EQ(style.borders[3].style, CssBorderStyle::Solid);
}

TEST_F(ChapterHtmlSlimParserTest, BorderedParagraphAddsABoxAfterItsText) {
  const XML_Char* attrs[] = {"style", "border: 2px solid; background-color: silver", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  EXPECT_EQ(parser.boxScopeCount, 1u);
  ChapterHtmlSlimParser::characterData(&parser, "Framed text", 11);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_EQ(parser.boxScopeCount, 0u);
  ASSERT_TRUE(parser.currentPage);
  EXPECT_EQ(std::count_if(parser.currentPage->elements.begin(), parser.currentPage->elements.end(),
                          [](const auto& item) { return item->getTag() == TAG_PageBorderBox; }),
            1);
}

TEST_F(ChapterHtmlSlimParserTest, InlineFontSizesMeasureAndRestoreWithinOneLine) {
  renderer.scalableBaseSize = 12;
  renderer.textAdvancePerChar = 6;
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Body ", 5);
  const XML_Char* attrs[] = {"style", "font-size: 2em", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Large", 5);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " tail", 5);
  parser.flushPartWordBuffer();
  ASSERT_EQ(parser.currentTextBlock->wordFontSizes.size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->wordFontSizes[0], 0);
  EXPECT_EQ(parser.currentTextBlock->wordFontSizes[1], 24);
  EXPECT_EQ(parser.currentTextBlock->wordFontSizes[2], 0);
  parser.makePages();
  ASSERT_EQ(parser.currentPage->elements.size(), 1u);
  const auto& line = static_cast<PageLine&>(*parser.currentPage->elements[0]);
  EXPECT_EQ(line.getBlock()->wordFontSize(1), 24);
  EXPECT_EQ(line.getBlock()->getBlockStyle().lineHeight, 48);
}

TEST_F(ChapterHtmlSlimParserTest, NestedInlineSizesUseParentAndRestore) {
  renderer.scalableBaseSize = 12;
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const XML_Char* big[] = {"style", "font-size: 2em", nullptr};
  const XML_Char* small[] = {"style", "font-size: 50%", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "span", big);
  EXPECT_EQ(parser.effectiveInlineFontSize, 24);
  ChapterHtmlSlimParser::startElement(&parser, "span", small);
  EXPECT_EQ(parser.effectiveInlineFontSize, 12);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  EXPECT_EQ(parser.effectiveInlineFontSize, 24);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  EXPECT_EQ(parser.effectiveInlineFontSize, 0);
}

TEST_F(ChapterHtmlSlimParserTest, CssHiddenIdsDoNotBecomeAnchorsOrPageMarkers) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("#hidden-chapter", CssParser::parseInlineStyle("display: none")));
  parser.tocAnchors.push_back("hidden-chapter");
  const XML_Char* attrs[] = {"id", "hidden-chapter", "role", "doc-pagebreak", "title", "42", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "h1", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "Hidden", 6);
  ChapterHtmlSlimParser::endElement(&parser, "h1");
  EXPECT_TRUE(parser.pendingAnchorId.empty());
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  EXPECT_TRUE(parser.anchorData.empty());
  EXPECT_EQ(parser.completedPageCount, 0);
  ChapterHtmlSlimParser::endElement(&parser, "p");
}

TEST_F(ChapterHtmlSlimParserTest, BorderedParagraphKeepsClosingPaddingAndMargin) {
  const XML_Char* attrs[] = {"style", "border:2px solid;padding-bottom:20px;margin-bottom:20px", nullptr};
  ChapterHtmlSlimParser::startElement(&parser, "p", attrs);
  ChapterHtmlSlimParser::characterData(&parser, "A", 1);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ASSERT_TRUE(parser.currentPage);
  int textBottom = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() == TAG_PageLine) textBottom = element->yPos + renderer.getLineHeight(0);
  }
  EXPECT_GE(parser.currentPageNextY, textBottom + 42);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "B", 1);
  parser.flushPartWordBuffer();
  parser.makePages();
  const auto& last = parser.currentPage->elements.back();
  ASSERT_EQ(last->getTag(), TAG_PageLine);
  EXPECT_GE(last->yPos, textBottom + 42);
}

TEST_F(ChapterHtmlSlimParserTest, ChildSelectorKeepsAlternativeAncestorMatches) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("section > div p", CssParser::parseInlineStyle("font-weight:bold")));
  const std::vector<CssAncestorEntry> ancestors{{0, "section", "", ""}, {1, "div", "", ""}, {2, "div", "", ""}};
  EXPECT_EQ(cssParser.resolveStyle("p", "", ancestors).fontWeight, CssFontWeight::Bold);
}

TEST_F(ChapterHtmlSlimParserTest, StandaloneInitialLetterPreservesWordBoundary) {
  ASSERT_TRUE(cssParser.processRuleBlockWithStyle("p::first-letter", CssParser::parseInlineStyle("initial-letter:3")));
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "I", 1);
  ChapterHtmlSlimParser::characterData(&parser, " am here", 8);
  parser.flushPartWordBuffer();
  EXPECT_EQ(parser.dropCap.length, 0);
  ASSERT_EQ(parser.currentTextBlock->words.size(), 3u);
  EXPECT_EQ(parser.currentTextBlock->words[0], "I");
  EXPECT_EQ(parser.currentTextBlock->words[1], "am");
  EXPECT_EQ(parser.currentTextBlock->words[2], "here");
}
