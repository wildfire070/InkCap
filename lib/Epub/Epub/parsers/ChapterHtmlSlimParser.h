#pragma once

#include <Arena.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/EpubRenderMode.h"
#include "Epub/FontSizeLadder.h"
#include "Epub/FootnoteEntry.h"
#include "Epub/Page.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"
#include "Epub/tables/CompactTableLayout.h"

class GfxRenderer;
class Epub;
#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
 public:
  enum class ParseStatus { More, Done, Error };

 private:
  static constexpr uint8_t MAX_SIMPLE_TABLE_COLUMNS = 8;
  static constexpr uint16_t MAX_SIMPLE_TABLE_CELL_WORDS = 160;
  static constexpr uint8_t TABLE_CELL_PADDING = 6;
  static constexpr size_t MAX_INLINE_STYLE_DEPTH = 64;
  static constexpr size_t MAX_BLOCK_STYLE_DEPTH = 16;
  // ancestorStack_ (below) grows one heap-string-holding entry per open tag
  // in the chapter's XHTML, which is fully attacker-controlled EPUB content
  // -- a crafted chapter with a long run of nested tags would otherwise grow
  // it without bound, aborting the device on allocation failure under this
  // build's -fno-exceptions (same bug class as MAX_INLINE_STYLE_DEPTH above,
  // and as DictHtmlRenderer::kMaxTagNesting). Entries past this depth are
  // simply not pushed; the close-tag handler's "pop ancestor entries at or
  // below the new depth" loop needs no matching bookkeeping change, since it
  // pops by comparing each entry's own stored depth against the current
  // depth, not a per-close-tag counter.
  static constexpr size_t MAX_CSS_ANCESTOR_DEPTH = 64;
  // Nested bordered blocks (e.g. a bordered <div> containing a bordered
  // <blockquote>) are rare in real EPUBs; a small cap keeps borderBoxStack_'s
  // footprint negligible while still covering realistic nesting.
  static constexpr size_t MAX_BORDER_BOX_DEPTH = 4;
  // List nesting beyond this is essentially unheard of in fiction EPUBs; a
  // small cap keeps listStack_'s footprint negligible either way.
  static constexpr size_t MAX_LIST_NESTING = 8;

  Epub* epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int skipEndElementStateUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int strikethroughUntilDepth = INT_MAX;
  int headingDepth = -1;
  bool headingOpenerActive = false;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t visibleTextOffset = 0;
  uint16_t currentTextRunBytes = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  bool forceParagraphIndents;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool guideReadingEnabled;
  uint8_t wordSpacing;
  CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;
  bool lowMemoryImageFallback = false;
  bool lowMemoryAbort = false;
  bool attemptedTextLayoutFontCacheRelease = false;
  EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault;
  std::string previewAnchor;
  uint16_t previewMaxPages = 0;
  bool previewAnchorFound = false;
  bool previewStopRequested = false;
  // Element ordinals (1-based, counting every startElement) used to start a footnote preview at the
  // block enclosing the anchor rather than at the anchor itself. 0 means "no block located".
  uint32_t previewStartOrdinal = 0;
  uint32_t previewElementOrdinal = 0;
  bool malformedMarkupTruncated = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  XML_Parser activeParser = nullptr;
  FsFile parseFile_;
  size_t parseFileOffset_ = 0;
  size_t parseFileSize_ = 0;
  uint32_t parseStartTime_ = 0;

  bool ensureInputFileOpen();

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasStrikethrough = false, strikethrough = false;
    bool hasBackgroundBlack = false, backgroundBlack = false;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasSmallCaps = false, smallCaps = false;
  };
  // Arena-backed style stacks. Initialized in parseAndBuildPages(); pointers are
  // null before and after each parse. StyleStackEntry and BlockStyle are trivially
  // destructible, so clear() on the arena is sufficient cleanup.
  Arena parseArena_;
  StyleStackEntry* inlineStyleBuf_ = nullptr;
  size_t inlineStyleCount_ = 0;
  BlockStyle* blockStyleBuf_ = nullptr;
  size_t blockStyleCount_ = 0;

  // Tracks CSS border boxes (see PageCssBorderBox in Page.h) currently open
  // across the block-style stack, depth-tagged the same way blockStyleBuf_
  // is. Pushing a scope (beginCssBorderBoxIfNeeded) is pure bookkeeping --
  // it does NOT touch currentPage/currentPageNextY, since a tag-open handler
  // has no guarantee a page exists yet (pages are created lazily). `elem` is
  // null until addLineToPage() actually materializes a PageCssBorderBox the
  // first time this block places a line on a page; on a page break,
  // finalizeOpenBorderBoxesForPageBreak() finalizes the old box's height and
  // clears `elem` back to null so the next addLineToPage() call rematerializes
  // an independent box on the new page -- so a bordered block spanning a page
  // break needs no cross-page height tracking beyond this small fixed stack.
  // MAX_BORDER_BOX_DEPTH=4 entries, plain members rather than arena-backed
  // like the larger inline/block style stacks above, since the footprint is
  // already negligible.
  struct BorderBoxScope {
    int depth = 0;
    int16_t x = 0;
    int16_t y = 0;  // valid only once elem != nullptr
    int16_t width = 0;
    bool borderTop = false;
    bool borderRight = false;
    bool borderBottom = false;
    bool borderLeft = false;
    PageCssBorderBox* elem = nullptr;  // owned by currentPage->elements
    // Set by endCssBorderBoxIfNeeded() when the block's close tag is reached,
    // but the scope is NOT popped yet: this block's text is still sitting
    // unflushed in currentTextBlock at that point (flushed lazily, only once
    // the NEXT startNewTextBlock() call's makePages() runs) -- see
    // finalizePendingCloseBorderBoxes()'s doc comment for why the actual pop
    // has to wait until that flush actually happens.
    bool pendingClose = false;
  };
  BorderBoxScope borderBoxStack_[MAX_BORDER_BOX_DEPTH];
  size_t borderBoxCount_ = 0;

  // Tracks whether the nearest open <ol>/<ul> ancestor is ordered, and (if
  // so) the running item count, depth-tagged the same way blockStyleBuf_ is.
  // <li> consults the top entry to decide bullet ("*") vs "N." numbering.
  struct ListMarkerContext {
    int depth = 0;
    bool ordered = false;
    uint16_t counter = 0;
  };
  ListMarkerContext listStack_[MAX_LIST_NESTING];
  size_t listStackCount_ = 0;

  // The body font's sibling sizes (see FontSizeLadder.h), set via
  // setFontSizeLadder() before parsing starts; empty (all-default) unless the
  // caller supplied one. auxFontId_ is the ONE non-body font this chapter may
  // additionally hold resident, budgeted against FontDecompressor's 4 page
  // slots (body regular/bold/italic + one auxiliary) -- the first block whose
  // resolved font differs from the body claims this slot; any later block
  // that would need a DIFFERENT font just keeps its own body-font rendering
  // instead (no crisp swap, but never a second aux slot).
  FontSizeLadder fontSizeLadder_;
  int32_t auxFontId_ = 0;

  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveStrikethrough = false;
  bool effectiveBackgroundBlack = false;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;
  bool effectiveSmallCaps = false;

  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    std::vector<std::pair<int, FootnoteEntry>> footnotes;
    uint32_t visibleTextOffset = 0;
    bool isHeader = false;
    uint8_t colSpan = 1;
  };

  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool hasHeaderCell = false;
    bool hasDataCell = false;
    uint16_t effectiveColumnCount = 0;
  };

  struct BufferedTable {
    BlockStyle blockStyle;
    std::vector<BufferedTableRow> rows;
    uint16_t maxCols = 0;
    uint16_t totalCells = 0;
    bool unsupported = false;
    // When the whole-table reservation is unavailable, retain only the current
    // source row plus the render-ready rows that fit on the active page.
    bool streaming = false;
    bool streamingFlattened = false;
    bool streamingTopSpacingApplied = false;
    uint8_t streamingColumnCount = 0;
    uint8_t streamingFragmentColumnCount = 0;
    uint16_t streamingFragmentHeight = 1;
    uint32_t streamingFragmentVisibleOffset = 0;
    std::vector<TableFragmentRow> streamingFragmentRows;
    std::vector<FootnoteEntry> streamingFragmentFootnotes;
  };

  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;
  int pendingListMarkerDepth = -1;
  bool currentTableCellIsHeader = false;
  uint8_t currentTableCellColSpan = 1;
  uint32_t currentTableCellVisibleOffset = 0;
  std::unique_ptr<BufferedTable> currentTableBuffer = nullptr;
  std::unique_ptr<CompactTableLayout> currentCompactTable = nullptr;
  bool compactTableFlattened = false;
  bool compactTableUnsupported = false;
  bool compactTableTopSpacingApplied = false;
  uint8_t compactFragmentColumnCount = 0;
  uint16_t compactFragmentHeight = 1;
  uint32_t compactFragmentVisibleOffset = 0;
  std::vector<TableFragmentRow> compactFragmentRows;
  std::vector<FootnoteEntry> compactFragmentFootnotes;
  std::vector<CssAncestorEntry> ancestorStack_;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  bool pendingAnchorFromInlineA = false;
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  uint16_t currentTextBlockParagraphIndex = 0;
  uint16_t currentTextBlockListItemIndex = 0;
  uint16_t currentPageParagraphIndex = 0;
  uint16_t currentPageListItemIndex = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  uint8_t nextFootnoteLinkId = 1;
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  struct PendingPublisherPageMarker {
    int wordIndex = 0;
    char label[16] = {};
  };
  std::vector<PendingPublisherPageMarker> pendingPublisherPageMarkers;

  void updateEffectiveInlineStyle();
  void skipCurrentElement();
  void skipDescendantsOfCurrentElement();
  bool shouldAbortForLowMemory(const char* stage);
  bool startNewPage(const char* reason);
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void addPendingPublisherPageMarker(const char* label);
  void attachPendingPublisherPageMarkers(int yPos);
  void flushPartWordBuffer();
  void flushLongTextRunIfNeeded(bool force = false);
  size_t bufferedWordsBeforeLayoutLimit() const;
  uint16_t textRunBytesBeforeLayoutLimit() const;
  void markCurrentPageFromCurrentTextBlock();
  void markCurrentPageFromCurrentElement();
  void setCurrentPageVisibleOffset(uint32_t offset);
  void completeCurrentPage();
  void makePages();
  int effectiveLineHeight() const;
  int effectiveLineHeight(int fontIdForLine) const;
  // Narrows `effectiveWidth` for a block that will render at
  // fontSizeResidualScale != 1.0 (see BlockStyle.h / resolveBlockFont): laying
  // out unscaled text against width/scale, then rendering the result scaled,
  // is exactly equivalent to laying it out at native scale, without touching
  // any of ParsedText's word-wrap/hyphenation/ruby measurement code. A no-op
  // (returns effectiveWidth unchanged) once the block has a real headingFontId
  // or no residual scale is set.
  uint16_t layoutWidthForBlock(const BlockStyle& blockStyle, uint16_t effectiveWidth) const;
  bool isPreviewBuild() const { return !previewAnchor.empty() && previewMaxPages > 0; }
  bool isScanningForPreviewAnchor() const { return isPreviewBuild() && !previewAnchorFound; }
  bool handlePreviewScanStart(const XML_Char** atts);
  void locatePreviewBlockStart();
  void startPreviewAtAnchor();
  void stopPreviewIfPageLimitReached();
  bool usesSimpleCssLookup() const { return renderMode != EpubRenderMode::CrossInkDefault; }
  bool flattensTables() const { return renderMode != EpubRenderMode::CrossInkDefault; }
  bool isLightMode() const { return renderMode == EpubRenderMode::Light; }
  bool honorsPublisherDecorations() const { return renderMode != EpubRenderMode::Light; }
  void pushCssAncestor(int depth, const char* tag, std::string_view classAttr);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applySmallCapsToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  void beginCssBorderBoxIfNeeded(const BlockStyle& blockStyle);
  void endCssBorderBoxIfNeeded();
  void resolveBlockFont(BlockStyle& blockStyle);
  void materializeOpenBorderBoxesIfNeeded();
  void finalizeOpenBorderBoxesForPageBreak();
  void finalizePendingCloseBorderBoxes();
  void finalizeCurrentTableCell();
  void emitBufferedTableAsParagraphs(BufferedTable& table);
  void emitBufferedTableAsFragments(BufferedTable& table);
  bool streamCurrentTableRow();
  bool flushStreamingTableFragment(BufferedTable& table);
  void emitStreamingTableRowsAsParagraphs(BufferedTable& table);
  void finishStreamingTable(BufferedTable& table);
  bool flushCompactTableFragment();
  bool emitCompactTableRow(TableFragmentRow& row, std::vector<std::shared_ptr<TextBlock>>& flatLines,
                           const std::vector<FootnoteEntry>& footnotes, uint32_t visibleTextOffset,
                           uint8_t fragmentColumnCount, bool flatten);
  void finishCompactTable();
  void fallbackStreamingTableToParagraphs(const char* reason);
  void emitCurrentTableBuffer();
  void fallbackCurrentTableBufferToParagraphs(const char* reason);
  void flushMalformedPartialContent();
  bool appendMalformedMarkupWarningPage();
  void prewarmSectionAdvanceTable(FsFile& file) const;
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      Epub& epub, const std::string& filepath, GfxRenderer& renderer, const int fontId, const float lineCompression,
      const bool extraParagraphSpacing, const bool forceParagraphIndents, const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled, const bool guideReadingEnabled, const uint8_t wordSpacing,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, CssParser* cssParser = nullptr,
      const EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault, std::string previewAnchor = {},
      const uint16_t previewMaxPages = 0)

      : epub(&epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        wordSpacing(wordSpacing > 4 ? 4 : wordSpacing),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        renderMode(renderMode),
        previewAnchor(std::move(previewAnchor)),
        previewMaxPages(previewMaxPages),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();
  // Must be called before parsing starts if block-level font-size resolution
  // is wanted (see resolveBlockFont). Deliberately not a constructor
  // parameter -- ReaderRenderSpec::fontSizeLadder is optional, settings-
  // derived data the caller may not always have on hand, and this keeps the
  // already-long positional constructor unchanged.
  void setFontSizeLadder(const FontSizeLadder& ladder) { fontSizeLadder_ = ladder; }
  bool parseAndBuildPages();
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)
  void releaseInputFile();

  void addLineToPage(std::shared_ptr<TextBlock> line, uint32_t visibleOffset);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  bool wasLowMemoryFallbackTriggered() const { return lowMemoryImageFallback; }
  bool wasLowMemoryAbortTriggered() const { return lowMemoryAbort; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : parseFileOffset_; }
  size_t parseTotalBytes() { return parseFileSize_; }
};
