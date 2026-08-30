#pragma once
#include <Epub.h>

#include <memory>

class GfxRenderer;

/**
 * Silent KOReader progress push while reading, with no UI interaction --
 * gated by CrossPointSettings::koreaderAutosyncMode (off / every chapter /
 * every 5% / every 10% / on exit). Independent of BookFusion's ProgressAutoSync:
 * separate credentials, separate document identity, either can be on without
 * the other.
 *
 * Push-only, with a guard: before pushing, fetches the remote position and
 * skips the push if remote is already further ahead than local (progress made
 * on another device) -- pushing local in that case would silently regress the
 * remote position. This never applies a remote position either way; that
 * stays a manual, user-confirmed action in KOReaderSyncActivity.
 *
 * Deliberately synchronous on the caller's task, same reasoning as
 * ProgressAutoSync: a background FreeRTOS task can't reliably get the
 * contiguous heap a TLS handshake needs while the reader holds a chapter's
 * layout, so this runs inline from EpubReaderActivity's own loop() at a
 * moment it chooses to be safe.
 */
namespace KOReaderAutoSync {

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

// Fires the pending push, if armed and CrossPointSettings::koreaderAutosyncMode
// isn't AUTOSYNC_OFF. epub must still be loaded -- computing the KOReader
// xpath position requires streaming the current spine item's XHTML, so this
// must run before the caller releases it. bookPercent is only used for the
// threshold-crossing dedup bookkeeping (matching what armIfThresholdCrossed()
// was armed with); the remote-ahead guard and the push itself use a
// percentage computed fresh from spineIndex/pageNumber/pageCount via
// ProgressMapper, since that's what actually gets sent. Connects WiFi
// silently (reusing the last-connected saved network), fetches remote
// progress to guard against regressing a further-ahead remote position,
// pushes if the guard passes, then disconnects if this call brought WiFi up
// itself. Clears the armed flag whether or not the push succeeded (a failed
// push is retried on the next threshold crossing, not spun on).
//
// Caller is responsible for choosing a safe moment (see maybeRunAutoSync()'s
// precedent in EpubReaderActivity.cpp): no render in flight, no page-turn
// input this tick, framebuffer available.
void runIfArmed(GfxRenderer& renderer, const std::shared_ptr<Epub>& epub, int spineIndex, int pageNumber,
                int pageCount, float bookPercent);

// AUTOSYNC_ON_EXIT's trigger: pushes unconditionally (if credentials are set
// and the mode is ON_EXIT), ignoring the threshold-crossing state. No
// bookPercent parameter needed here -- on-exit ignores the threshold/dedup
// bookkeeping entirely, and the push itself always computes its own
// percentage (see runIfArmed()'s comment).
void runOnExit(GfxRenderer& renderer, const std::shared_ptr<Epub>& epub, int spineIndex, int pageNumber,
              int pageCount);

}  // namespace KOReaderAutoSync
