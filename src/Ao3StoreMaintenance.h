#pragma once

// Opportunistic self-heal for the two path-keyed AO3 queues (Marked-for-Later,
// New Chapters), analogous to RECENT_BOOKS.pruneMissing() in
// LibraryActivity::onEnter(). Call this wherever those queues are about to be
// displayed (Ao3LibraryActivity::onEnter()) to clean up entries left stale by
// something the firmware can't observe directly -- USB Mass Storage deletion
// from a host computer, Calibre wireless removal, editing the SD card on
// another device. Normal in-app delete is already covered directly by
// BookMetadataUtils::clearFileMetadata(); this is the safety net for
// everything else.
void selfHealAo3PathStores();
