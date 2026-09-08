#pragma once

#include <SecureHttpClient.h>

#include <string>

#include "KOReaderResponseCap.h"

// SecureHttpClient::GET()/sendRequest()'s default (no-callback) overloads
// buffer the ENTIRE response body into an unbounded std::string, regardless
// of Content-Length, and regardless of whether the caller ever reads it
// back -- the growth happens inside the request itself, before it returns.
// That's fine for BookFusion/AO3, which always talk to a fixed, trusted
// endpoint, but KOReader sync points at a server the USER configures
// (self-hosted or third-party), so a misbehaving or malicious server can
// grow that buffer without limit and abort the firmware (this project
// builds with -fno-exceptions, so a failed allocation is a hard crash, not
// a catchable exception).
//
// These wrappers replace the default overloads with a bounded DataCallback
// (see KOReaderResponseCap.h for the actual bounds-check logic and its
// host-side unit test) so KOReaderSyncClient.cpp never needs to call the
// unbounded GET()/sendRequest() overloads directly. Deliberately a NEW file
// rather than a change inside KOReaderSyncClient.cpp's own request
// functions: this file (lib/KOReaderSync/) is synced from upstream CrossInk
// from time to time (see git history for prior instances of an upstream
// sync silently reverting a local fix in this same client), and a brand new
// file a future sync has no reason to touch is far more durable than a
// change buried inside a function upstream also edits. If you're touching
// KOReaderSyncClient.cpp's authenticate()/createUser()/getProgress()/
// updateProgress() during a CrossInk sync and don't see a call into
// koreader_sync::boundedGet()/boundedSendRequest() anymore, the sync
// silently dropped this fix -- reapply it rather than assuming it's fine.
namespace koreader_sync {

// Bounded equivalent of http.GET(): populates outBody (cleared first) up to
// MAX_RESPONSE_BYTES, aborting the transfer if the server sends more. Note:
// if the cap is hit, the underlying SecureHttpClient still returns the
// already-parsed HTTP status code (not a negative error code) -- the
// connection is closed rather than reused, and outBody is left truncated at
// the cap. A caller that deserializeJson()s the (now-truncated) body already
// treats the resulting parse failure as an error; a caller that never reads
// the body doesn't need to do anything else -- it only ever needed the
// transfer to not crash the device.
int boundedGet(freeink::SecureHttpClient& http, std::string& outBody);

// Bounded equivalent of http.sendRequest(method, payload). Same truncation
// contract as boundedGet() above.
int boundedSendRequest(freeink::SecureHttpClient& http, const char* method, const std::string& payload,
                       std::string& outBody);

}  // namespace koreader_sync
