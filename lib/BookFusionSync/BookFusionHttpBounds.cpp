#include "BookFusionHttpBounds.h"

#include <Logging.h>

namespace bookfusion_sync {

namespace {
bool appendBounded(std::string& body, const uint8_t* data, size_t len, size_t maxBytes = MAX_RESPONSE_BYTES) {
  if (body.size() + len > maxBytes) {
    LOG_ERR("BFS", "Response body exceeded %zu bytes, aborting transfer", maxBytes);
    return false;
  }
  body.append(reinterpret_cast<const char*>(data), len);
  return true;
}
}  // namespace

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

}  // namespace bookfusion_sync
