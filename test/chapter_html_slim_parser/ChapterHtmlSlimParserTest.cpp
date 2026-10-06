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
