# Standalone CLI diagnostic tool (not a gtest target -- ctest ignores it).
# Runs a real chapter of extracted EPUB content through the actual
# production CSS/HTML/layout/render pipeline and dumps BMP page images, to
# visually verify the CSS-gaps/font-size work against real books.
add_executable(EpubRenderPreview
  EpubRenderPreview.cpp
  LinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/lib/RestartHooks/RestartHooks.cpp
)
target_include_directories(EpubRenderPreview PRIVATE
  stubs
  ${REPO_ROOT}/test/epub_grayscale/raster_stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/builtinFonts
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Epub/Epub/tables
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/RestartHooks
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
  ${REPO_ROOT}/test/memory_policy/stubs
)
target_compile_definitions(EpubRenderPreview PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(EpubRenderPreview PRIVATE crosspoint_test_common expat)
