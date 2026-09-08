#include "KOReaderResponseCap.h"

#include <Logging.h>

namespace koreader_sync {

bool appendBounded(std::string& body, const uint8_t* data, size_t len, size_t maxBytes) {
  if (body.size() + len > maxBytes) {
    LOG_ERR("KOSync", "Response body exceeded %zu bytes, aborting transfer", maxBytes);
    return false;
  }
  body.append(reinterpret_cast<const char*>(data), len);
  return true;
}

}  // namespace koreader_sync
