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
    bool ok = text.layoutAndExtractLines(renderer, font, 240,
                                         [&](std::shared_ptr<TextBlock> block, uint32_t offset, uint32_t) {
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

namespace {
struct SpacingLayout {
  size_t lines = 0;
  bool allStamped = true;
};

SpacingLayout layoutWithCharacterSpacing(const int8_t spacing) {
  ParsedText text(false, false, false, false, false, 0, BlockStyle(), false, spacing);
  const std::vector<std::string> words = {"A", "paragraph", "with", "different", "word", "lengths", "and", "text."};
  for (int i = 0; i < 200; ++i) text.addWord(words[i % words.size()], EpdFontFamily::REGULAR);
  GfxRenderer renderer;
  SpacingLayout result;
  EXPECT_TRUE(text.layoutAndExtractLines(renderer, 0, 240, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
    ++result.lines;
    if (block->getBlockStyle().characterSpacing != spacing) result.allStamped = false;
  }));
  return result;
}
}  // namespace

TEST_F(TextLayoutBackingTest, CharacterSpacingChangesLineBreaksAndStampsEveryLine) {
  const auto normal = layoutWithCharacterSpacing(0);
  const auto wide = layoutWithCharacterSpacing(2);
  const auto tight = layoutWithCharacterSpacing(-2);
  ASSERT_GT(normal.lines, 1U);
  // Wider glyph gaps fill lines sooner; tighter gaps fit more words per line.
  EXPECT_GT(wide.lines, normal.lines);
  EXPECT_LT(tight.lines, normal.lines);
  EXPECT_TRUE(normal.allStamped);
  EXPECT_TRUE(wide.allStamped);
  EXPECT_TRUE(tight.allStamped);
}

TEST_F(TextLayoutBackingTest, SerializeRoundTripsCharacterSpacing) {
  // Cached lines reload without re-running layout, so the spacing they were drawn with must persist.
  for (const int8_t spacing : {int8_t{-2}, int8_t{0}, int8_t{2}}) {
    BlockStyle style;
    style.characterSpacing = spacing;
    TextBlock block({"Title"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, {}, {}, {}, style);
    ASSERT_TRUE(block.valid());

    FsFile output;
    ASSERT_TRUE(Storage.openFileForWrite("test", "spacing", output));
    ASSERT_TRUE(block.serialize(output));
    output.close();

    FsFile input;
    ASSERT_TRUE(Storage.openFileForRead("test", "spacing", input));
    auto reloaded = TextBlock::deserialize(input);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->getBlockStyle().characterSpacing, spacing);
  }
}
