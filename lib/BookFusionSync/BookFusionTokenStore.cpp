#include "BookFusionTokenStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

void BookFusionTokenStore::toJson(JsonDocument& doc) const {
  doc["accessToken_obf"] = obfuscation::obfuscateToBase64(accessToken);
}

bool BookFusionTokenStore::fromJson(JsonVariantConst doc) {
  obfuscation::DecodeStatus accessStatus = obfuscation::DecodeStatus::INVALID;
  accessToken = obfuscation::deobfuscateFromBase64(doc["accessToken_obf"] | "", &accessStatus);
  if (accessStatus == obfuscation::DecodeStatus::LEGACY && !accessToken.empty()) {
    LOG_DBG("BFS", "Resaving BookFusion token to update format");
    requestResave();
  }
  if (accessStatus == obfuscation::DecodeStatus::INVALID && !accessToken.empty()) {
    LOG_ERR("BFS", "Ignoring unreadable BookFusion access token");
    accessToken.clear();
  }

  return true;
}

void BookFusionTokenStore::setTokens(const std::string& access) { accessToken = access; }

bool BookFusionTokenStore::hasToken() const {
  ensureLoaded();
  return !accessToken.empty();
}

void BookFusionTokenStore::clearTokens() {
  accessToken.clear();
  saveToFile();
}
