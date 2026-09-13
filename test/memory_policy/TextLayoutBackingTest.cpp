#include <Arduino.h>
#include <Epub/ParsedText.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>

// Hyphen dictionary availability is outside allocation policy. Exercise the
// production non-hyphenating layout, real bidi, TextBlock and serialization.
std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

struct TextLayoutBackingTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  std::vector<uint8_t> layout(int font, int scenario, bool* result = nullptr) {
    BlockStyle style;
    if (scenario == 1) {
      style.directionDefined = true;
      style.isRtl = true;
    }
    if (scenario == 3) {
      style.marginLeft = 12;
      style.marginRight = 12;
    }
    ParsedText text(false, false, false, scenario == 4, scenario == 5, 0, style);
    const std::vector<std::string> words =
        scenario == 1 ? std::vector<std::string>{"שלום", "עולם", "עברית", "123"}
        : scenario == 2
            ? std::vector<std::string>{"日", "本", "語", "読", "書"}
            : std::vector<std::string>{"A", "paragraph", "with", "different", "word", "lengths", "and", "punctuation."};
    for (int i = 0; i < 400; ++i)
      text.addWord(words[i % words.size()], i % 3 ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD, false, scenario == 2,
                   false, 0, i * 8);
    if (scenario == 2) text.setRubyGroupAt(0, 2, "にほん");
    FsFile output;
    EXPECT_TRUE(Storage.openFileForWrite("test", "layout", output));
    GfxRenderer renderer;
    bool ok = text.layoutAndExtractLines(renderer, font, 240, [&](std::shared_ptr<TextBlock> block, uint32_t offset) {
      EXPECT_TRUE(block->valid());
      EXPECT_TRUE(block->serialize(output));
      EXPECT_EQ(output.write(&offset, sizeof(offset)), sizeof(offset));
    });
    output.close();
    if (result)
      *result = ok;
    else
      EXPECT_TRUE(ok);
    return Storage.bytes("layout");
  }
};
TEST_F(TextLayoutBackingTest, SameSerializedLinesAcrossPoolsFontsAndReadingAids) {
  std::ofstream reference;
  if (const char* path = std::getenv("LAYOUT_REFERENCE_OUT")) reference.open(path, std::ios::binary);
  for (int font : {0, 2, 5})
    for (int scenario = 0; scenario < 6; ++scenario) {
      fakeheap::reset(false);
      const auto expected = layout(font, scenario);
      ASSERT_FALSE(expected.empty());
      EXPECT_TRUE(fakeheap::live.empty());
      if (reference) reference.write(reinterpret_cast<const char*>(expected.data()), expected.size());
      fakeheap::reset();
      EXPECT_EQ(layout(font, scenario), expected);
      EXPECT_TRUE(fakeheap::live.empty());
      fakeheap::reset();
      fakeheap::external.fail = 1000;
      EXPECT_EQ(layout(font, scenario), expected);
      EXPECT_TRUE(fakeheap::live.empty());
    }
}
#ifndef PSRAM_REFERENCE
TEST_F(TextLayoutBackingTest, AllocationFailureReturnsWithoutPartialLinesOrLeaks) {
  fakeheap::external.fail = 1000;
  fakeheap::internal.largest = 1;
  bool ok = true;
  auto output = layout(0, 0, &ok);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(output.empty());
  EXPECT_TRUE(fakeheap::live.empty());
}
#endif

TEST_F(TextLayoutBackingTest, SerializeRoundTripsBlockLevelFontSizeResolution) {
  // A cached section reloads TextBlocks directly, without re-running
  // ChapterHtmlSlimParser::resolveBlockFont() -- these fields must survive
  // serialize/deserialize or a reopened book silently loses block-level
  // font-size resolution until the cache is next rebuilt.
  BlockStyle style;
  style.fontSizeMultiplier = 1.75f;
  style.headingFontId = 424242;
  style.fontResolved = true;
  style.fontSizeResidualScale = 1.3f;

  TextBlock block({"Title"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, {}, {}, {}, style);
  ASSERT_TRUE(block.valid());

  FsFile output;
  ASSERT_TRUE(Storage.openFileForWrite("test", "fontsize", output));
  ASSERT_TRUE(block.serialize(output));
  output.close();

  FsFile input;
  ASSERT_TRUE(Storage.openFileForRead("test", "fontsize", input));
  auto reloaded = TextBlock::deserialize(input);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_FLOAT_EQ(reloaded->getBlockStyle().fontSizeMultiplier, 1.75f);
  EXPECT_EQ(reloaded->getBlockStyle().headingFontId, 424242);
  EXPECT_TRUE(reloaded->getBlockStyle().fontResolved);
  EXPECT_FLOAT_EQ(reloaded->getBlockStyle().fontSizeResidualScale, 1.3f);
}
