#pragma once

#include <string>
#include <vector>

// Support for AO3 Receive (the Send to AvesO3 browser extension's target).
//
// Received files always land in their own folder and are recorded in a pending
// list; after the post-transfer restart the AO3 library indexes each one, looks
// for an existing copy by AO3 work ID (filenames can't match: AO3 names its
// downloads by title only, Calibre/FanFicFare use the user's own template), and
// asks before replacing anything.
namespace Ao3ReceiveUtils {

// Created on demand, like BookFusion's download folder. Deliberately independent of
// the AO3 library folder, which users may share with other sources.
constexpr char DEFAULT_RECEIVE_FOLDER[] = "/AO3 Downloads";

// "receiveFolder" from /.crosspoint/ao3_settings.json, or DEFAULT_RECEIVE_FOLDER.
// Always has a leading '/', never a trailing one.
std::string receiveFolder();

// folder + "/" + fileName, or "<name> (2).<ext>", "<name> (3).<ext>", ... if that
// path is taken. Empty string if no free name was found.
std::string uniqueFilePath(const std::string& folder, const std::string& fileName);

// Pending list of received fics the device hasn't reviewed yet. Persisted on the SD
// card because the transfer always ends in a silent restart.
void appendPending(const std::string& path);
std::vector<std::string> readPending();
void removePending(const std::string& path);
bool hasPending();

// Replaces oldPath with the file at newPath, keeping oldPath's reading progress,
// bookmarks and status: the old copy is set aside first and only deleted once the new
// one is in place, the new file's cache/index entry is dropped, and the old path's
// derived cache is cleared (which also tombstones its AO3 index record).
bool replaceExisting(const std::string& oldPath, const std::string& newPath);

}  // namespace Ao3ReceiveUtils
