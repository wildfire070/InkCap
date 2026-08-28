#include "BookFusionSyncClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#ifdef SIMULATOR
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#include <StreamingJsonParser.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "BookFusionTokenStore.h"

int BookFusionSyncClient::lastHttpCode = 0;
int BookFusionSyncClient::lastTransportError = 0;

namespace {
constexpr char BASE_URL[] = "https://www.bookfusion.com";

// BookFusion's device-code endpoint is the same one KOReader's official
// BookFusion cloud-storage plugin uses; "koreader" is that plugin's public
// OAuth client_id, not a secret. CrossInk already speaks the KOReader/
// crosspoint-sync protocol family (see lib/KOReaderSync/), so identifying as
// this client for BookFusion's device flow is consistent with what CrossInk
// already is, not spoofing another app.
//
// NOTE: this endpoint/field shape was not independently verified against
// BookFusion's live API docs -- it comes from a now-stale reference fork
// (see plan doc) and matches the standard RFC 8628 device-flow shape. Sanity
// check against BookFusion's current docs before shipping.
constexpr char CLIENT_ID[] = "koreader";
constexpr char DEVICE_CODE_GRANT_TYPE[] = "urn:ietf:params:oauth:grant-type:device_code";
constexpr char API_ACCEPT[] = "application/json; api_version=10";

const char* classifyJsonBody(const char* body) {
  if (!body || body[0] == '\0') return "empty response";
  const char* cursor = body;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
  if (*cursor == '\0') return "blank response";
  if (*cursor == '<') return "HTML response";
  if (*cursor != '{' && *cursor != '[') return "non-JSON response";
  return "malformed JSON";
}

void logJsonParseFailure(const char* context, DeserializationError error, const char* body) {
  char preview[97];
  size_t i = 0;
  if (body) {
    for (; i < sizeof(preview) - 1 && body[i] != '\0'; i++) {
      const char c = body[i];
      preview[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
  }
  preview[i] = '\0';
  LOG_ERR("BFS", "%s JSON parse failed: %s (%s, preview=\"%s\")", context, error.c_str(), classifyJsonBody(body),
          preview);
}

// Same heap floor KOReaderSyncClient uses before a TLS handshake -- both
// clients run over the same wolfSSL transport on-device.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("BFS", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}

// Total-Count is authoritative when present. Falling back to "the page came
// back full" avoids under-reporting hasMore on an old server that doesn't
// send the header, at the cost of one extra (empty) page fetch at the very
// end of the list in that fallback case.
bool deriveHasMore(int page, int totalCount, size_t bookCount) {
  return (totalCount > 0) ? (page + 1) * BOOKFUSION_BOOKS_PER_PAGE < totalCount
                          : bookCount == static_cast<size_t>(BOOKFUSION_BOOKS_PER_PAGE);
}

#ifdef SIMULATOR
void addAuthHeaders(HTTPClient& http) {
  const std::string bearer = "Bearer " + BOOKFUSION_STORE.getAccessToken();
  http.addHeader("Authorization", bearer.c_str());
  http.addHeader("Accept", API_ACCEPT);
}
#else
void addAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Authorization", std::string("Bearer ") + BOOKFUSION_STORE.getAccessToken());
  http.addHeader("Accept", API_ACCEPT);
}

// Persistent client kept alive across a browse session (see beginSession()).
// Owned by a unique_ptr so endSession()/re-entrant beginSession() cleanup is
// automatic; a raw new would need a matching explicit delete on every exit
// path.
std::unique_ptr<freeink::SecureHttpClient> s_sessionClient;

// Returns the session client if beginSession() is active, else initializes
// and returns the caller's local fallback. Kept in one place so a future
// caller can't forget the fallback's setInsecure() call.
freeink::SecureHttpClient& resolveClient(freeink::SecureHttpClient& fallback) {
  if (s_sessionClient) return *s_sessionClient;
  fallback.setInsecure();
  return fallback;
}

void safeCopy(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  const size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

// SAX consumer for POST /api/user/books/search's top-level array of book
// objects. Feeds straight off the wire via StreamingJsonParser (the same
// parser OtaUpdater's ReleaseJsonParser already uses for GitHub's release
// JSON) so a page response never needs a full-buffer allocation or a
// round-trip through an SD temp file. Only fields BookFusionBook actually
// has are captured; every other field BookFusion sends (cover, format,
// categories, ...) is structurally skipped via depth tracking, not parsed.
class BookFusionSearchJsonStream {
 public:
  explicit BookFusionSearchJsonStream(BookFusionSearchResult& out)
      : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                             sOnArrayStart, sOnArrayEnd}),
        out(out) {
    out.books.clear();
    out.books.reserve(BOOKFUSION_BOOKS_PER_PAGE);
  }

  void feed(const char* data, size_t len) { parser.feed(data, len); }
  bool ok() const { return !parser.hasError(); }

 private:
  enum class Position : uint8_t { TOP_LEVEL, IN_BOOK, IN_AUTHORS_ARRAY, IN_AUTHOR_OBJECT };
  enum class LastKey : uint8_t { NONE, ID, TITLE, AUTHORS, AUTHOR_NAME };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitBook();

  StreamingJsonParser parser;
  BookFusionSearchResult& out;

  Position position = Position::TOP_LEVEL;
  LastKey lastKey = LastKey::NONE;
  uint8_t bookDepth = 0;    // nesting depth within the book object currently open; 1 = the book's own fields
  uint8_t authorDepth = 0;  // nesting depth within the author object currently open

  BookFusionBook current;
  bool haveId = false;
  char currentAuthorName[48];
};

void BookFusionSearchJsonStream::commitBook() {
  if (haveId && static_cast<int>(out.books.size()) < BOOKFUSION_BOOKS_PER_PAGE) {
    if (current.title.empty()) current.title = "Untitled";
    out.books.push_back(std::move(current));
  }
  current = BookFusionBook{};
  haveId = false;
}

void BookFusionSearchJsonStream::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  switch (self->position) {
    case Position::IN_BOOK:
      if (self->bookDepth == 1) {
        if (len == 2 && memcmp(key, "id", 2) == 0)
          self->lastKey = LastKey::ID;
        else if (len == 5 && memcmp(key, "title", 5) == 0)
          self->lastKey = LastKey::TITLE;
        else if (len == 7 && memcmp(key, "authors", 7) == 0)
          self->lastKey = LastKey::AUTHORS;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_AUTHOR_OBJECT:
      if (self->authorDepth == 1) {
        self->lastKey = (len == 4 && memcmp(key, "name", 4) == 0) ? LastKey::AUTHOR_NAME : LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void BookFusionSearchJsonStream::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  switch (self->lastKey) {
    case LastKey::TITLE:
      if (self->position == Position::IN_BOOK && self->bookDepth == 1) self->current.title.assign(value, len);
      break;
    case LastKey::AUTHOR_NAME:
      if (self->position == Position::IN_AUTHOR_OBJECT && self->authorDepth == 1)
        safeCopy(self->currentAuthorName, sizeof(self->currentAuthorName), value, len);
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  if (self->lastKey == LastKey::ID && self->position == Position::IN_BOOK && self->bookDepth == 1) {
    self->current.bookId = static_cast<uint32_t>(strtoul(value, nullptr, 10));
    self->haveId = self->current.bookId != 0;
  }
  self->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnBool(void* ctx, bool /*value*/) {
  static_cast<BookFusionSearchJsonStream*>(ctx)->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnNull(void* ctx) {
  static_cast<BookFusionSearchJsonStream*>(ctx)->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnObjectStart(void* ctx) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  switch (self->position) {
    case Position::TOP_LEVEL:
      self->position = Position::IN_BOOK;
      self->bookDepth = 1;
      self->current = BookFusionBook{};
      self->haveId = false;
      break;
    case Position::IN_BOOK:
      self->bookDepth++;  // an object field we don't care about (cover, etc.) -- skip structurally
      break;
    case Position::IN_AUTHORS_ARRAY:
      self->position = Position::IN_AUTHOR_OBJECT;
      self->authorDepth = 1;
      self->currentAuthorName[0] = '\0';
      break;
    case Position::IN_AUTHOR_OBJECT:
      self->authorDepth++;
      break;
  }
  self->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  switch (self->position) {
    case Position::IN_BOOK:
      if (self->bookDepth > 0) self->bookDepth--;
      if (self->bookDepth == 0) {
        self->commitBook();
        self->position = Position::TOP_LEVEL;
      }
      break;
    case Position::IN_AUTHOR_OBJECT:
      if (self->authorDepth > 0) self->authorDepth--;
      if (self->authorDepth == 0) {
        if (self->currentAuthorName[0] != '\0') {
          if (!self->current.author.empty()) self->current.author += ", ";
          self->current.author += self->currentAuthorName;
        }
        self->position = Position::IN_AUTHORS_ARRAY;
      }
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnArrayStart(void* ctx) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  switch (self->position) {
    case Position::TOP_LEVEL:
      break;  // the root array itself; stay at TOP_LEVEL, next object starts a book
    case Position::IN_BOOK:
      if (self->lastKey == LastKey::AUTHORS && self->bookDepth == 1) {
        self->position = Position::IN_AUTHORS_ARRAY;
      } else {
        self->bookDepth++;  // an array field we don't care about -- skip structurally
      }
      break;
    case Position::IN_AUTHORS_ARRAY:
    case Position::IN_AUTHOR_OBJECT:
      self->authorDepth++;
      break;
  }
  self->lastKey = LastKey::NONE;
}

void BookFusionSearchJsonStream::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<BookFusionSearchJsonStream*>(ctx);
  switch (self->position) {
    case Position::TOP_LEVEL:
      break;  // end of the root array
    case Position::IN_BOOK:
      if (self->bookDepth > 0) self->bookDepth--;
      break;
    case Position::IN_AUTHORS_ARRAY:
      self->position = Position::IN_BOOK;
      break;
    case Position::IN_AUTHOR_OBJECT:
      if (self->authorDepth > 0) self->authorDepth--;
      break;
  }
}
#endif
}  // namespace

void BookFusionSyncClient::beginSession() {
#ifndef SIMULATOR
  if (s_sessionClient) {
    LOG_DBG("BFS", "Session already active");
    return;
  }
  s_sessionClient = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!s_sessionClient) {
    LOG_ERR("BFS", "OOM starting BookFusion session; falling back to per-request connections");
    return;
  }
  s_sessionClient->setInsecure();
  LOG_DBG("BFS", "Session started");
#endif
}

void BookFusionSyncClient::endSession() {
#ifndef SIMULATOR
  if (!s_sessionClient) return;
  s_sessionClient.reset();
  LOG_DBG("BFS", "Session ended");
#endif
}

BookFusionSyncClient::Error BookFusionSyncClient::startDeviceAuth(BookFusionDeviceAuth& outAuth) {
  lastHttpCode = 0;
  lastTransportError = 0;

  const std::string url = std::string(BASE_URL) + "/api/user/auth/device";
  LOG_DBG("BFS", "Requesting device code: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["client_id"] = CLIENT_ID;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "startDeviceAuth response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("startDeviceAuth", error, responseBody.c_str());
    return JSON_ERROR;
  }

  outAuth.deviceCode = doc["device_code"] | "";
  outAuth.userCode = doc["user_code"] | "";
  outAuth.verificationUri = doc["verification_uri"] | "";
  outAuth.verificationUriComplete = doc["verification_uri_complete"] | "";
  outAuth.interval = doc["interval"] | 5;
  outAuth.expiresIn = doc["expires_in"] | 600;

  if (outAuth.deviceCode.empty() || outAuth.userCode.empty()) {
    LOG_ERR("BFS", "startDeviceAuth: missing device_code/user_code in response");
    return JSON_ERROR;
  }

  LOG_DBG("BFS", "Device code received: user_code=%s, interval=%ds, expires_in=%ds", outAuth.userCode.c_str(),
          outAuth.interval, outAuth.expiresIn);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::pollForToken(const std::string& deviceCode) {
  lastHttpCode = 0;
  lastTransportError = 0;

  const std::string url = std::string(BASE_URL) + "/api/user/auth/token";
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["grant_type"] = DEVICE_CODE_GRANT_TYPE;
  reqDoc["client_id"] = CLIENT_ID;
  reqDoc["device_code"] = deviceCode;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "pollForToken response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("pollForToken", error, responseBody.c_str());
    return JSON_ERROR;
  }

  if (httpCode == 200) {
    const char* token = doc["access_token"] | "";
    if (token[0] == '\0') return JSON_ERROR;
    BOOKFUSION_STORE.setTokens(token);
    BOOKFUSION_STORE.saveToFile();
    LOG_DBG("BFS", "Token received and saved");
    return OK;
  }

  const char* errCode = doc["error"] | "";
  LOG_DBG("BFS", "pollForToken error: %s", errCode);

  if (std::strcmp(errCode, "authorization_pending") == 0) return AUTH_PENDING;
  if (std::strcmp(errCode, "slow_down") == 0) return SLOW_DOWN;
  if (std::strcmp(errCode, "expired_token") == 0) return EXPIRED;
  if (std::strcmp(errCode, "access_denied") == 0) return AUTH_FAILED;
  // BookFusion has been observed returning "invalid_grant" (HTTP 400) while
  // authorization is still pending, which is non-standard but matches what
  // KOReader's own BookFusion plugin tolerates -- treat any unrecognized
  // error the same way rather than giving up early.
  return AUTH_PENDING;
}

BookFusionSyncClient::Error BookFusionSyncClient::searchBooks(int page, const char* list, BookFusionSearchResult& out) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  const std::string url = std::string(BASE_URL) + "/api/user/books/search";
  LOG_DBG("BFS", "searchBooks page=%d (heap: %u)", page, (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["page"] = page;
  reqDoc["per_page"] = BOOKFUSION_BOOKS_PER_PAGE;
  reqDoc["sort"] = "added_at-desc";
  if (list != nullptr) reqDoc["list"] = list;
  std::string body;
  serializeJson(reqDoc, body);

  int totalCount = 0;

#ifdef SIMULATOR
  // Discard every field but the ones we display; BookFusion book objects
  // carry ~20 fields (cover URLs, descriptions, etc.) that would otherwise
  // multiply JsonDocument heap use several times over for no benefit here.
  // (Only needed here -- the on-device branch below streams straight into
  // BookFusionSearchJsonStream and never buffers a JsonDocument at all.)
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["authors"][0]["name"] = true;
  JsonDocument doc;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  // The simulator's HTTPClient mock doesn't expose response headers, so
  // totalCount stays 0 here and deriveHasMore() falls back to its
  // page-came-back-full heuristic -- fine for a desktop dev build, no
  // production pagination accuracy needed.
  http.end();

  LOG_DBG("BFS", "searchBooks page=%d response: %d", page, httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode != 200) return SERVER_ERROR;

  const DeserializationError error = deserializeJson(doc, responseBody.c_str(), DeserializationOption::Filter(filter));
  if (error) {
    logJsonParseFailure("searchBooks", error, responseBody.c_str());
    return JSON_ERROR;
  }

  if (!doc.is<JsonArray>()) {
    LOG_ERR("BFS", "searchBooks: expected a JSON array");
    return JSON_ERROR;
  }

  out.books.clear();
  out.books.reserve(BOOKFUSION_BOOKS_PER_PAGE);
  out.currentPage = page;
  out.totalCount = totalCount;

  for (JsonObject book : doc.as<JsonArray>()) {
    // Safety cap only -- per_page already asks the server for exactly
    // BOOKFUSION_BOOKS_PER_PAGE, so this shouldn't trigger in practice.
    if (static_cast<int>(out.books.size()) >= BOOKFUSION_BOOKS_PER_PAGE) break;

    BookFusionBook b;
    b.bookId = book["id"] | (uint32_t)0;
    if (b.bookId == 0) continue;
    b.title = book["title"] | "Untitled";

    std::string authors;
    for (JsonObject author : book["authors"].as<JsonArray>()) {
      const char* name = author["name"] | "";
      if (name[0] == '\0') continue;
      if (!authors.empty()) authors += ", ";
      authors += name;
    }
    b.author = std::move(authors);

    out.books.push_back(std::move(b));
  }

  out.hasMore = deriveHasMore(page, totalCount, out.books.size());
  LOG_DBG("BFS", "searchBooks: %zu books on page %d, hasMore=%d, totalCount=%d", out.books.size(), page, out.hasMore,
          totalCount);
  return OK;
#else
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient& http = resolveClient(tmp);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  // Feed the response straight into a SAX parser as it streams off the
  // wire, instead of buffering it (each wolfSSL handshake fragments the
  // heap -- see beginSession() -- so a large response's std::string::append()
  // growing past what the fragmented heap can offer calls abort(), no
  // exceptions on this firmware) or round-tripping it through an SD temp
  // file. BookFusionSearchJsonStream populates out.books directly.
  BookFusionSearchJsonStream stream(out);
  const int httpCode =
      http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                       [&stream](const uint8_t* data, size_t len) -> bool {
                         stream.feed(reinterpret_cast<const char*>(data), len);
                         return true;
                       });
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string totalCountHeader = http.getHeader("Total-Count");
  if (!totalCountHeader.empty()) totalCount = std::atoi(totalCountHeader.c_str());

  LOG_DBG("BFS", "searchBooks page=%d response: %d", page, httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode != 200) return SERVER_ERROR;
  if (!stream.ok()) {
    LOG_ERR("BFS", "searchBooks: malformed JSON response");
    return JSON_ERROR;
  }

  out.currentPage = page;
  out.totalCount = totalCount;
  out.hasMore = deriveHasMore(page, totalCount, out.books.size());
  LOG_DBG("BFS", "searchBooks: %zu books on page %d, hasMore=%d, totalCount=%d", out.books.size(), page, out.hasMore,
          totalCount);
  return OK;
#endif
}

BookFusionSyncClient::Error BookFusionSyncClient::getProgress(uint32_t bookId, BookFusionProgress& outProgress) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  char urlBuf[128];
  snprintf(urlBuf, sizeof(urlBuf), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  const std::string url = urlBuf;
  LOG_DBG("BFS", "getProgress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
  http.end();
#endif

  LOG_DBG("BFS", "getProgress response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("getProgress", error, responseBody.c_str());
    return JSON_ERROR;
  }

  outProgress.bookId = bookId;
  outProgress.percentage = doc["percentage"] | 0.0f;
  outProgress.timestamp = doc["updated_at"] | (int64_t)0;

  LOG_DBG("BFS", "Remote progress: %.2f%%", outProgress.percentage * 100);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::updateProgress(const BookFusionProgress& progress) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  char urlBuf[128];
  snprintf(urlBuf, sizeof(urlBuf), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)progress.bookId);
  const std::string url = urlBuf;
  LOG_DBG("BFS", "updateProgress: %s (%.2f%%)", url.c_str(), progress.percentage * 100);
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument reqDoc;
  reqDoc["percentage"] = progress.percentage;
  std::string body;
  serializeJson(reqDoc, body);

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();
#endif

  LOG_DBG("BFS", "updateProgress response: %d", httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 200 || httpCode == 201) return OK;
  return SERVER_ERROR;
}

BookFusionSyncClient::Error BookFusionSyncClient::getDownloadUrl(uint32_t bookId, std::string& outUrl) {
  if (!BOOKFUSION_STORE.hasToken()) return NO_TOKEN;

  char urlBuf[128];
  snprintf(urlBuf, sizeof(urlBuf), "%s/api/user/books/%lu/download", BASE_URL, (unsigned long)bookId);
  const std::string url = urlBuf;
  LOG_DBG("BFS", "getDownloadUrl: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST("{}");
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const String responseBody = http.getString();
  http.end();
#else
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient& http = resolveClient(tmp);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", "{}");
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  const std::string responseBody = http.getString();
#endif

  LOG_DBG("BFS", "getDownloadUrl book=%lu response: %d", (unsigned long)bookId, httpCode);
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 403 || httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, responseBody.c_str());
  if (error) {
    logJsonParseFailure("getDownloadUrl", error, responseBody.c_str());
    return JSON_ERROR;
  }

  outUrl = doc["url"] | "";
  if (outUrl.empty()) {
    LOG_ERR("BFS", "getDownloadUrl: missing url field");
    return JSON_ERROR;
  }
  return OK;
}

std::string BookFusionSyncClient::getBearerToken() { return BOOKFUSION_STORE.getAccessToken(); }

std::string BookFusionSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return tr(STR_BF_NO_TOKEN_MSG);
    case NETWORK_ERROR:
      return tr(STR_BF_NETWORK_ERROR);
    case AUTH_FAILED:
      return tr(STR_BF_AUTH_REJECTED);
    case AUTH_PENDING:
      return tr(STR_BF_AUTH_PENDING);
    case SLOW_DOWN:
      return tr(STR_BF_AUTH_PENDING);
    case EXPIRED:
      return tr(STR_BF_AUTH_EXPIRED);
    case SERVER_ERROR:
      if (lastHttpCode > 0) {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), tr(STR_BF_HTTP_STATUS_FORMAT), lastHttpCode);
        return std::string(buffer);
      }
      return tr(STR_BF_SERVER_ERROR);
    case JSON_ERROR:
      return tr(STR_BF_BAD_RESPONSE);
    case NOT_FOUND:
      return tr(STR_BF_NOT_FOUND);
    case LOW_MEMORY:
      return tr(STR_BF_LOW_MEMORY);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
