#include "KOReaderHttpBounds.h"

namespace koreader_sync {

int boundedGet(freeink::SecureHttpClient& http, std::string& outBody) {
  outBody.clear();
  return http.GET([&outBody](const uint8_t* data, size_t len) { return appendBounded(outBody, data, len); });
}

int boundedSendRequest(freeink::SecureHttpClient& http, const char* method, const std::string& payload,
                       std::string& outBody) {
  outBody.clear();
  return http.sendRequest(
      method, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
      [&outBody](const uint8_t* data, size_t len) { return appendBounded(outBody, data, len); });
}

}  // namespace koreader_sync
