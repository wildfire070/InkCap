#pragma once
#include <Print.h>
#include <expat.h>

#include <string>

class BookMetadataCache;

class TocNcxParser final : public Print {
  enum ParserState { START, IN_NCX, IN_NAV_MAP, IN_NAV_POINT, IN_NAV_LABEL, IN_NAV_LABEL_TEXT, IN_CONTENT };

  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;

  std::string currentLabel;
  std::string currentSrc;
  // Wide enough that open/close balance (increment on <navPoint>, decrement
  // and zero-check on </navPoint>) stays correct no matter how deep a
  // crafted NCX nests -- a uint8_t here would wrap past 255 levels, throwing
  // off the very zero-check this depends on to know when nesting has fully
  // unwound. The value actually stored (BookMetadataCache::createTocEntry's
  // `level` parameter) is clamped separately to fit its own uint8_t; this
  // counter's only job is tracking depth correctly, not fitting in a byte.
  uint32_t currentDepth = 0;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  explicit TocNcxParser(const std::string& baseContentPath, const size_t xmlSize, BookMetadataCache* cache)
      : baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~TocNcxParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
