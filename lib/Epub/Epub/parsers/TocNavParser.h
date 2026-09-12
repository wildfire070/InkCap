#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

class BookMetadataCache;

// Parser for EPUB 3 nav.xhtml navigation documents
// Parses HTML5 nav elements with epub:type="toc" to extract table of contents
class TocNavParser final : public Print {
  enum ParserState {
    START,
    IN_HTML,
    IN_BODY,
    IN_NAV_TOC,  // Inside <nav epub:type="toc">
    IN_OL,       // Inside <ol>
    IN_LI,       // Inside <li>
    IN_ANCHOR,   // Inside <a>
  };

  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;

  // Track nesting depth for <ol> elements to determine TOC depth. Wide
  // enough that open/close balance stays correct no matter how deep a
  // crafted nav document nests -- a uint8_t here would wrap past 255
  // levels, throwing off the zero-check this depends on to know when
  // nesting has fully unwound. The value actually stored
  // (BookMetadataCache::createTocEntry's `level` parameter) is clamped
  // separately to fit its own uint8_t; this counter's only job is tracking
  // depth correctly, not fitting in a byte.
  uint32_t olDepth = 0;
  // Current entry data being collected
  std::string currentLabel;
  std::string currentHref;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  explicit TocNavParser(const std::string& baseContentPath, const size_t xmlSize, BookMetadataCache* cache)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~TocNavParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
