#pragma once

/**
 * Shared WiFi reconnect helper for silent background sync pushes (BookFusion
 * and KOReader auto-sync). Reuses only the last-connected saved network,
 * blocking for up to ~8s -- no scan, no fallback to other saved networks:
 * this is opportunistic background sync, so if the usual network isn't
 * reachable right now, the caller should skip this round rather than hunt
 * for another one.
 */
namespace SilentAutoSyncWifi {

// Returns true if a station WiFi connection is available (already connected,
// or freshly brought up here). broughtUpWifi is set to true only when this
// call itself brought the connection up, so the caller knows whether it's
// responsible for tearing it down afterward via teardown().
bool connect(bool& broughtUpWifi);

// Disconnects and powers down the WiFi radio. Only call this if connect()
// reported broughtUpWifi=true -- otherwise WiFi was already up for some
// other reason and isn't this caller's to tear down.
void teardown();

}  // namespace SilentAutoSyncWifi
