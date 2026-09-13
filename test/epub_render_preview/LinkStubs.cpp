// Narrow link-time stubs for the pieces of the real EPUB pipeline this tool
// deliberately doesn't exercise: image decoding (test content is chosen to
// have no <img> tags) and compact-table layout (not the focus of this CSS/
// font-size verification pass). Everything else -- CssParser,
// ChapterHtmlSlimParser, ParsedText, TextBlock, Page (PageLine/
// PageCssBorderBox/PageHorizontalRule/PageImage), ImageBlock, GfxRenderer,
// BidiUtils -- is the real, production implementation.
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/PreviewBlockLocator.h>
#include <Epub/tables/CompactTableLayout.h>

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }

PreviewBlockLocator::PreviewBlockLocator(const char*, IsBlockTagFn) {}
PreviewBlockLocator::~PreviewBlockLocator() = default;
bool PreviewBlockLocator::feed(const char*, int, bool) { return false; }

CompactTableLayout::CompactTableLayout(GfxRenderer& renderer, int, uint16_t, uint16_t, uint16_t, uint8_t,
                                       BlockStyle tableStyle)
    : renderer_(renderer), tableStyle_(tableStyle) {}
bool CompactTableLayout::beginRow() { return true; }
bool CompactTableLayout::beginCell(bool, uint8_t, uint32_t, const BlockStyle&) { return true; }
bool CompactTableLayout::appendWord(std::string_view, EpdFontFamily::Style, bool, bool, uint8_t) { return true; }
bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>&) { return true; }
CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow&, std::vector<std::shared_ptr<TextBlock>>&,
                                                            std::vector<FootnoteEntry>&, uint32_t&) {
  return RowResult::Ok;
}
