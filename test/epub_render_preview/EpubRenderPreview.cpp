// Standalone CLI tool (not a gtest) that runs a REAL chapter of extracted
// EPUB content through the actual production pipeline -- CssParser,
// ChapterHtmlSlimParser, ParsedText, TextBlock, Page, GfxRenderer, real
// built-in fonts -- and dumps each resulting page as a BMP image, so the
// CSS-gaps/font-size work can be visually verified against real books
// instead of just unit-tested against synthetic fixtures.
//
// Usage:
//   EpubRenderPreview <chapter.xhtml> <stylesheet.css> <output-prefix>
//                     [--sd-font] [--width W] [--height H] [--bitter]
//
// --sd-font simulates reading with a custom SD-card font: the body font is
// registered under an id the FontSizeLadder never contains, so any
// font-size CSS on this content exercises the residual-scale fallback
// (GfxRenderer::drawTextScaled) instead of the ladder's crisp font swap.
// Without it, the body font is one of 4 real built-in sizes and font-size
// CSS exercises the ladder's normal real-font-resource path.
//
// Pages are written as <output-prefix>-0.bmp, -1.bmp, ...

#include <Epub.h>
#include <Epub/FontSizeLadder.h>
#include <Epub/Page.h>
#include <Epub/css/CssParser.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "lexenddeca_10_bold.h"
#include "lexenddeca_10_bolditalic.h"
#include "lexenddeca_10_italic.h"
#include "lexenddeca_10_regular.h"
#include "lexenddeca_12_bold.h"
#include "lexenddeca_12_bolditalic.h"
#include "lexenddeca_12_italic.h"
#include "lexenddeca_12_regular.h"
#include "lexenddeca_14_bold.h"
#include "lexenddeca_14_bolditalic.h"
#include "lexenddeca_14_italic.h"
#include "lexenddeca_14_regular.h"
#include "lexenddeca_16_bold.h"
#include "lexenddeca_16_bolditalic.h"
#include "lexenddeca_16_italic.h"
#include "lexenddeca_16_regular.h"

#include "bitter_10_bold.h"
#include "bitter_10_bolditalic.h"
#include "bitter_10_italic.h"
#include "bitter_10_regular.h"
#include "bitter_12_bold.h"
#include "bitter_12_bolditalic.h"
#include "bitter_12_italic.h"
#include "bitter_12_regular.h"
#include "bitter_14_bold.h"
#include "bitter_14_bolditalic.h"
#include "bitter_14_italic.h"
#include "bitter_14_regular.h"
#include "bitter_16_bold.h"
#include "bitter_16_bolditalic.h"
#include "bitter_16_italic.h"
#include "bitter_16_regular.h"

namespace {

// Distinct from every real on-device font id (which are derived from a hash
// of the font family+size string, all far outside this tiny range) so there
// is no risk of accidental collision in this standalone tool.
constexpr int kFontId10 = 1;
constexpr int kFontId12 = 2;
constexpr int kFontId14 = 3;
constexpr int kFontId16 = 4;
constexpr int kSdFontId = 999;  // never added to the ladder -- simulates an SD-card font

// Minimal uncompressed 24-bit BMP writer for a 1bpp MSB-first row-major
// buffer (GfxRenderer's BW framebuffer layout, see HalDisplay::bw).
void writeBmp(const std::string& path, const uint8_t* bw, int width, int height, int strideBytes) {
  const int rowSize = (width * 3 + 3) & ~3;
  const int pixelDataSize = rowSize * height;
  const int fileSize = 54 + pixelDataSize;

  std::vector<uint8_t> header(54, 0);
  auto put16 = [&](int offset, uint16_t value) { std::memcpy(header.data() + offset, &value, 2); };
  auto put32 = [&](int offset, uint32_t value) { std::memcpy(header.data() + offset, &value, 4); };
  header[0] = 'B';
  header[1] = 'M';
  put32(2, fileSize);
  put32(10, 54);
  put32(14, 40);
  put32(18, width);
  put32(22, height);
  put16(26, 1);
  put16(28, 24);
  put32(34, pixelDataSize);

  std::vector<uint8_t> pixels(pixelDataSize, 0xFF);
  for (int y = 0; y < height; ++y) {
    // BMP rows are bottom-up.
    const int srcY = height - 1 - y;
    for (int x = 0; x < width; ++x) {
      const int bitIndex = x;
      const uint8_t byte = bw[srcY * strideBytes + (bitIndex >> 3)];
      const bool white = (byte >> (7 - (bitIndex & 7))) & 1;
      const uint8_t value = white ? 0xFF : 0x00;
      const int dst = y * rowSize + x * 3;
      pixels[dst] = pixels[dst + 1] = pixels[dst + 2] = value;
    }
  }

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
  out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "Usage: " << argv[0]
              << " <chapter.xhtml> <stylesheet.css> <output-prefix> [--sd-font] [--width W] [--height H] [--bitter]\n";
    return 1;
  }
  const std::string chapterPath = argv[1];
  const std::string cssPath = argv[2];
  const std::string outPrefix = argv[3];
  bool sdFont = false;
  bool useBitter = false;
  int width = 480;
  int height = 800;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--sd-font") {
      sdFont = true;
    } else if (arg == "--bitter") {
      useBitter = true;
    } else if (arg == "--width" && i + 1 < argc) {
      width = std::atoi(argv[++i]);
    } else if (arg == "--height" && i + 1 < argc) {
      height = std::atoi(argv[++i]);
    }
  }

  HalDisplay display(width, height);
  GfxRenderer renderer(display);
  renderer.begin();
  // GfxRenderer::Portrait assumes the underlying HalDisplay buffer is the
  // device's native LANDSCAPE panel shape and rotates logical portrait
  // coordinates onto it -- our HalDisplay is already sized to the logical
  // viewport (width x height), so LandscapeCounterClockwise (the identity
  // mapping: phyX=x, phyY=y) is the correct "no rotation" orientation here.
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);

  EpdFont r10(useBitter ? &bitter_10_regular : &lexenddeca_10_regular);
  EpdFont b10(useBitter ? &bitter_10_bold : &lexenddeca_10_bold);
  EpdFont i10(useBitter ? &bitter_10_italic : &lexenddeca_10_italic);
  EpdFont bi10(useBitter ? &bitter_10_bolditalic : &lexenddeca_10_bolditalic);
  EpdFontFamily family10(&r10, &b10, &i10, &bi10);

  EpdFont r12(useBitter ? &bitter_12_regular : &lexenddeca_12_regular);
  EpdFont b12(useBitter ? &bitter_12_bold : &lexenddeca_12_bold);
  EpdFont i12(useBitter ? &bitter_12_italic : &lexenddeca_12_italic);
  EpdFont bi12(useBitter ? &bitter_12_bolditalic : &lexenddeca_12_bolditalic);
  EpdFontFamily family12(&r12, &b12, &i12, &bi12);

  EpdFont r14(useBitter ? &bitter_14_regular : &lexenddeca_14_regular);
  EpdFont b14(useBitter ? &bitter_14_bold : &lexenddeca_14_bold);
  EpdFont i14(useBitter ? &bitter_14_italic : &lexenddeca_14_italic);
  EpdFont bi14(useBitter ? &bitter_14_bolditalic : &lexenddeca_14_bolditalic);
  EpdFontFamily family14(&r14, &b14, &i14, &bi14);

  EpdFont r16(useBitter ? &bitter_16_regular : &lexenddeca_16_regular);
  EpdFont b16(useBitter ? &bitter_16_bold : &lexenddeca_16_bold);
  EpdFont i16(useBitter ? &bitter_16_italic : &lexenddeca_16_italic);
  EpdFont bi16(useBitter ? &bitter_16_bolditalic : &lexenddeca_16_bolditalic);
  EpdFontFamily family16(&r16, &b16, &i16, &bi16);

  renderer.insertFont(kFontId10, family10);
  renderer.insertFont(kFontId12, family12);
  renderer.insertFont(kFontId14, family14);
  renderer.insertFont(kFontId16, family16);
  if (sdFont) {
    // Same real glyphs, registered under an id the ladder below never
    // contains -- reproduces "reading with an SD font" for
    // ChapterHtmlSlimParser::resolveBlockFont() without needing a real
    // SdCardFont/.cpfont blob.
    renderer.insertFont(kSdFontId, family12);
  }

  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
  renderer.setFontCacheManager(&cache);
  FontDecompressor decompressor;
  if (!decompressor.init()) {
    std::cerr << "FontDecompressor::init() failed\n";
    return 1;
  }
  cache.setFontDecompressor(&decompressor);

  const int bodyFontId = sdFont ? kSdFontId : kFontId12;

  FontSizeLadder ladder;
  if (!sdFont) {
    ladder.addRung(kFontId10, 10 * 100 / 12);
    ladder.addRung(kFontId12, 12 * 100 / 12);
    ladder.addRung(kFontId14, 14 * 100 / 12);
    ladder.addRung(kFontId16, 16 * 100 / 12);
  }

  CssParser cssParser{"/tmp/epub_render_preview_css_cache"};
  {
    HalFile cssFile;
    if (!Storage.openFileForRead("PREVIEW", cssPath, cssFile)) {
      std::cerr << "Could not open stylesheet: " << cssPath << "\n";
      return 1;
    }
    if (!cssParser.loadFromStream(cssFile)) {
      std::cerr << "Warning: CSS parsing reported failure (continuing)\n";
    }
    cssFile.close();
  }

  Epub epub;
  std::vector<std::unique_ptr<Page>> completedPages;
  auto completePageFn = [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
    completedPages.push_back(std::move(page));
  };

  // paragraphAlignment=4 (CssTextAlign::None, "Book's Style") + embeddedStyle=true:
  // render the book's own CSS/HTML alignment as authored, not a forced reader
  // override -- this tool exists to verify fidelity to the source content.
  ChapterHtmlSlimParser parser{epub,
                               chapterPath,
                               renderer,
                               bodyFontId,
                               1.0f,
                               false,
                               false,
                               4,
                               static_cast<uint16_t>(width),
                               static_cast<uint16_t>(height),
                               false,
                               false,
                               false,
                               0,
                               completePageFn,
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};
  parser.setFontSizeLadder(ladder);

  if (!parser.parseAndBuildPages()) {
    std::cerr << "parseAndBuildPages() failed to open " << chapterPath << "\n";
    return 1;
  }
  if (!parser.finishParse()) {
    std::cerr << "Warning: finishParse() reported an error\n";
  }

  if (completedPages.empty() && parser.currentPage) {
    completedPages.push_back(std::move(parser.currentPage));
  }

  if (completedPages.empty()) {
    std::cerr << "No pages produced (empty chapter content?)\n";
    return 1;
  }

  std::cout << "Rendered " << completedPages.size() << " page(s) from " << chapterPath << " (body font "
            << (sdFont ? "SD-simulated 12pt" : "built-in 12pt") << (useBitter ? " Bitter" : " Lexend Deca") << ")\n";

  for (size_t i = 0; i < completedPages.size(); ++i) {
    display.clearScreen(0xFF);
    completedPages[i]->render(renderer, bodyFontId, 0, 0);
    const std::string outPath = outPrefix + "-" + std::to_string(i) + ".bmp";
    writeBmp(outPath, display.bw.data(), width, height, display.stride);
    std::cout << "  wrote " << outPath << "\n";
  }

  return 0;
}
