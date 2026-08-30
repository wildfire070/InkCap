#pragma once
#include <cstdint>
#include <string>

class GfxRenderer;

/**
 * Silent BookFusion progress push while reading, with no UI interaction --
 * gated by CrossPointSettings::autosyncMode (off / every chapter / every 5%
 * / every 10% / on exit).
 *
 * Push-only: this never applies a remote position, only uploads the local
 * one -- applying a position without the user looking at the screen could
 * silently jump them to the wrong page. That stays a manual, user-confirmed
 * action in BookFusionSyncActivity.
 *
 * Deliberately synchronous on the caller's task (blocks for the WiFi
 * connect + HTTP round trip, up to a few seconds): a background FreeRTOS
 * task can't reliably get the ~20-35KB contiguous heap block a TLS
 * handshake needs while the reader is actively holding a chapter's layout,
 * so this runs inline from EpubReaderActivity's own loop() at a moment it
 * chooses to be safe (see maybeRunAutoSync() there), the same way every
 * other BookFusion network call in this codebase blocks its caller's task.
 */
namespace ProgressAutoSync {

// Call once when a book is opened (EpubReaderActivity::onEnter()), so a
// threshold crossing from the *previous* book doesn't immediately fire here.
void resetSessionBaseline();

// Cheap, non-blocking: call on every forward page turn. Records whether the
// configured threshold (chapter boundary, or the 5%/10% step) was crossed
// since the last push. No I/O, no allocation.
void armIfThresholdCrossed(int spineIndex, float bookPercent);

// True if armIfThresholdCrossed() (or the on-exit trigger) has a push
// pending that hasn't run yet.
bool isArmed();

// Fires the pending push, if armed and CrossPointSettings::autosyncMode
// isn't AUTOSYNC_OFF. Connects WiFi silently (reusing the last-connected
// saved network; gives up quickly if that's not available -- no UI, no
// network scan), pushes percentage (+ chapter/page if available) and any
// unsynced reading-time delta, then disconnects if this call brought WiFi
// up itself. Clears the armed flag whether or not the push succeeded (a
// failed push is retried on the next threshold crossing, not spun on).
//
// Caller is responsible for choosing a safe moment (see the idle-prewarm
// precedent in EpubReaderActivity.cpp): no render in flight, no page-turn
// input this tick, framebuffer available. bookPercent/spineIndex/pageNumber
// /pageCount describe the position being pushed.
void runIfArmed(GfxRenderer& renderer, uint32_t bookId, const std::string& epubPath, const std::string& epubCachePath,
                float bookPercent, int spineIndex, int pageNumber, int pageCount, int spineCount);

// AUTOSYNC_ON_EXIT's trigger: pushes unconditionally (if there's a linked
// book and the mode is ON_EXIT), ignoring the threshold-crossing state.
// Same signature/behavior as runIfArmed() otherwise.
void runOnExit(GfxRenderer& renderer, uint32_t bookId, const std::string& epubPath, const std::string& epubCachePath,
              float bookPercent, int spineIndex, int pageNumber, int pageCount, int spineCount);

}  // namespace ProgressAutoSync
