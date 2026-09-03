#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

/**
 * Singleton class for storing the BookFusion OAuth device-flow access token
 * on the SD card. Obfuscated the same way KOReaderCredentialStore obfuscates
 * its password: XOR with the device's hardware MAC address, base64-encoded.
 *
 * No expiry is persisted: this device has no guaranteed wall-clock time (RTC
 * is optional hardware, see HalClock::isAvailable()), so a locally-computed
 * absolute expiry would be meaningless across a reboot. BookFusionSyncClient
 * instead treats any AUTH_FAILED response reactively and asks the user to
 * re-authenticate, the same pattern KOReaderSyncClient already uses.
 */
class BookFusionTokenStore : public PersistableStore<BookFusionTokenStore> {
 private:
  std::string accessToken;

  BookFusionTokenStore() = default;
  ~BookFusionTokenStore() = default;

  friend class PersistableStore<BookFusionTokenStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookfusion.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setTokens(const std::string& access);
  const std::string& getAccessToken() const {
    ensureLoaded();
    return accessToken;
  }

  bool hasToken() const;
  void clearTokens();
};

// Helper macro to access the token store
#define BOOKFUSION_STORE BookFusionTokenStore::getInstance()
