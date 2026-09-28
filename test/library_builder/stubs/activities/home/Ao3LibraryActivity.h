#pragma once

// Narrow stub: BookCacheUtils.cpp only calls the one static method below (to
// flag a post-transfer AO3 rescan on cache-clear when the book had AO3 info,
// which none of this test's fixtures do) -- the real header is a full
// Activity subclass pulling in ActivityManager/FreeRTOS, none of which this
// test has any need to compile against.
class Ao3LibraryActivity {
 public:
  static void requestTransferScan() {}
};
