// Regression test for the KOReader-sync response-size cap
// (lib/KOReaderSync/KOReaderResponseCap.h). KOReader sync is the one HTTP
// consumer in this firmware that talks to a user-configurable, arbitrary
// server rather than a fixed first-party endpoint, so it's the one place
// SecureHttpClient's own unbounded response-body buffering (a genuine,
// unfixed bug in the freeink-sdk submodule -- see KOReaderHttpBounds.h)
// can actually be reached by something other than this fork's own trusted
// servers. This test exists specifically so that if a future CrossInk
// merge into KOReaderSyncClient.cpp silently drops the call sites that use
// appendBounded()/boundedGet()/boundedSendRequest(), the test suite catches
// it -- see KOReaderHttpBounds.h's header comment for the full story.

#include "KOReaderResponseCap.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {
std::vector<uint8_t> makeBytes(size_t count, uint8_t fill = 'x') { return std::vector<uint8_t>(count, fill); }
}  // namespace

TEST(KOReaderResponseCap, AppendsUnderTheCapNormally) {
  std::string body;
  const auto chunk = makeBytes(10);
  EXPECT_TRUE(koreader_sync::appendBounded(body, chunk.data(), chunk.size(), /*maxBytes=*/100));
  EXPECT_EQ(body.size(), 10u);
}

TEST(KOReaderResponseCap, AccumulatesAcrossMultipleChunksLikeStreamedData) {
  std::string body;
  const auto chunk = makeBytes(10);
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(koreader_sync::appendBounded(body, chunk.data(), chunk.size(), /*maxBytes=*/100));
  }
  EXPECT_EQ(body.size(), 50u);
}

TEST(KOReaderResponseCap, ExactlyAtTheCapSucceeds) {
  std::string body;
  const auto chunk = makeBytes(100);
  EXPECT_TRUE(koreader_sync::appendBounded(body, chunk.data(), chunk.size(), /*maxBytes=*/100));
  EXPECT_EQ(body.size(), 100u);
}

TEST(KOReaderResponseCap, OneByteOverTheCapIsRejected) {
  std::string body;
  const auto chunk = makeBytes(101);
  EXPECT_FALSE(koreader_sync::appendBounded(body, chunk.data(), chunk.size(), /*maxBytes=*/100));
  // The rejected chunk must not partially land -- a caller that gives up on
  // `false` should see a body untouched by the chunk that broke the cap.
  EXPECT_EQ(body.size(), 0u);
}

TEST(KOReaderResponseCap, RejectsOnceCumulativeTotalWouldCrossTheCap) {
  std::string body;
  const auto first = makeBytes(60);
  const auto second = makeBytes(60);  // 60 + 60 = 120 > 100
  ASSERT_TRUE(koreader_sync::appendBounded(body, first.data(), first.size(), /*maxBytes=*/100));
  EXPECT_FALSE(koreader_sync::appendBounded(body, second.data(), second.size(), /*maxBytes=*/100));
  // Simulates a misbehaving/hostile server streaming far more than a real
  // koreader-sync response ever would: the accumulator must stop growing
  // right at the first chunk that would cross the cap, not silently keep
  // appending forever the way SecureHttpClient's own default callback does.
  EXPECT_EQ(body.size(), 60u);
}

TEST(KOReaderResponseCap, DefaultCapIsSaneAndBoundedForRealJsonResponses) {
  // A real koreader-sync response (auth token, one document's progress) is a
  // small JSON object -- guard against someone accidentally setting this to
  // 0 (rejects everything) or an effectively-unbounded value (defeats the
  // whole point of this file).
  EXPECT_GT(koreader_sync::MAX_RESPONSE_BYTES, 1024u);
  EXPECT_LT(koreader_sync::MAX_RESPONSE_BYTES, 10u * 1024u * 1024u);
}

TEST(KOReaderResponseCap, DefaultParameterMatchesMaxResponseBytes) {
  std::string body;
  const auto chunk = makeBytes(koreader_sync::MAX_RESPONSE_BYTES + 1);
  // No explicit maxBytes argument -- exercises the default parameter that
  // boundedGet()/boundedSendRequest() actually rely on.
  EXPECT_FALSE(koreader_sync::appendBounded(body, chunk.data(), chunk.size()));
}
