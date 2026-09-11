#pragma once

#include <SecureHttpClient.h>

#include <string>

// SecureHttpClient::GET()/sendRequest()'s default (no-callback) overloads
// buffer the ENTIRE response body into an unbounded std::string, regardless
// of Content-Length. Every BookFusionSyncClient call also uses
// setInsecure() (no TLS certificate validation), so a network-position
// attacker (rogue AP, ARP spoofing) can MITM the connection and return an
// arbitrarily large body to any of these endpoints -- this build uses
// -fno-exceptions, so the resulting failed std::string reallocation is a
// hard device abort(), not a catchable exception.
//
// These wrappers replace the default overloads with a bounded DataCallback
// so BookFusionSyncClient.cpp never needs to call the unbounded
// GET()/sendRequest() overloads directly. A standalone file/logic (not
// shared with lib/KOReaderSync/'s equivalent KOReaderHttpBounds.h, which
// solves the identical problem) deliberately: that directory is synced from
// upstream CrossInk from time to time, so cross-depending on it here would
// make this fix's fate tied to code this project doesn't control.
namespace bookfusion_sync {

// Real BookFusion responses (auth tokens, a book list page, one book's
// progress) are small JSON payloads. This is a deliberately loose ceiling
// meant only to catch a misbehaving/hostile response, not to constrain a
// legitimate one.
constexpr size_t MAX_RESPONSE_BYTES = 65536;

// Bounded equivalent of http.GET(): populates outBody (cleared first) up to
// MAX_RESPONSE_BYTES, aborting the transfer if the server sends more. If the
// cap is hit, the underlying SecureHttpClient still returns the
// already-parsed HTTP status code (not a negative error code) -- the
// connection is closed rather than reused, and outBody is left truncated at
// the cap. A caller that deserializes the (now-truncated) body already
// treats the resulting parse failure as an error.
int boundedGet(freeink::SecureHttpClient& http, std::string& outBody);

// Bounded equivalent of http.sendRequest(method, payload). Same truncation
// contract as boundedGet() above.
int boundedSendRequest(freeink::SecureHttpClient& http, const char* method, const std::string& payload,
                       std::string& outBody);

}  // namespace bookfusion_sync
