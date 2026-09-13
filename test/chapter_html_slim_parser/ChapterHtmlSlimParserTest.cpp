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
