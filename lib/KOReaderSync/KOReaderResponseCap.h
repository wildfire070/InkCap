#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Deliberately has NO dependency on SecureHttpClient/networking, so
// appendBounded() -- the one invariant that actually prevents the crash
// KOReaderHttpBounds.h exists for -- has a host-side unit test
// (test/koreader_http_bounds/) that doesn't need to mock a TLS connection.
// See KOReaderHttpBounds.h for the full story on why this exists as a
// separate, brand-new file rather than a change inside KOReaderSyncClient.cpp.
namespace koreader_sync {

// Real koreader-sync responses (auth tokens, a single document's progress)
// are small JSON objects, well under 1 KB even generously. This is a
// deliberately loose ceiling meant only to catch a misbehaving/hostile
// server's unbounded stream, not to constrain a legitimate response.
constexpr size_t MAX_RESPONSE_BYTES = 65536;

// Appends up to maxBytes total into body, refusing (and leaving body
// untouched) once that total would be exceeded.
bool appendBounded(std::string& body, const uint8_t* data, size_t len, size_t maxBytes = MAX_RESPONSE_BYTES);

}  // namespace koreader_sync
