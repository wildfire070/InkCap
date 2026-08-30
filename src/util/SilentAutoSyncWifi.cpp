#include "SilentAutoSyncWifi.h"

#include <Logging.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"
#include "network/WifiUtils.h"

namespace SilentAutoSyncWifi {

bool connect(bool& broughtUpWifi) {
  broughtUpWifi = false;
  if (hasActiveStationWifiConnection()) return true;

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_INF("AutoSync", "Skipping silent connect: no previously-connected WiFi network to reuse");
    return false;
  }
  const auto cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    LOG_INF("AutoSync", "Skipping silent connect: no saved credential for last-connected network %s",
            lastSsid.c_str());
    return false;
  }

  LOG_INF("AutoSync", "Silently connecting to %s", cred->ssid.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());

  constexpr int POLL_INTERVAL_MS = 100;
  constexpr int MAX_POLLS = 80;  // ~8s
  for (int i = 0; i < MAX_POLLS; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      broughtUpWifi = true;
      return true;
    }
    delay(POLL_INTERVAL_MS);
  }

  LOG_INF("AutoSync", "Silent connect to %s timed out; skipping this sync round", cred->ssid.c_str());
  WiFi.disconnect(true);
  return false;
}

void teardown() {
  WiFi.disconnect(true);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

}  // namespace SilentAutoSyncWifi
