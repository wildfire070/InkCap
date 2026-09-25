#pragma once

#include <string>
#include <vector>

#include <Epub/BookIds.h>

// Duplicate handling for books downloaded from BookFusion. A download never replaces or resumes
// over an existing file: it lands under a free name (" (2)", ...) and is recorded in a pending list
// on the SD card. After the post-download restart, DownloadReviewActivity looks for another copy of
// the same book (by BookFusion ID or AO3 work ID, never by filename), asks whether to replace it, and
// either moves the new file over the old one or leaves both.
namespace DownloadReview {

struct Entry {
  std::string path;              // the newly downloaded file
  std::string collisionOrigin;   // the existing file the download's plain name collided with, if any
};

// folder + "/" + fileName, or "<name> (2).<ext>", "<name> (3).<ext>", ... if taken. Empty if none free.
std::string uniqueFilePath(const std::string& folder, const std::string& fileName);

void appendPending(const std::string& path, const std::string& collisionOrigin);
std::vector<Entry> readPending();
void removePending(const std::string& path);
bool hasPending();

// Another copy of the book at entry.path: one that shares its BookFusion or AO3 ID, else the file its
// plain name collided with (same "<title> - <author>"). False when there is none.
bool findDuplicate(const Entry& entry, const BookIds::Ids& ids, std::string& oldPath);

// Replaces oldPath with the file at newPath, keeping oldPath's reading progress, bookmarks and status.
bool replaceExisting(const std::string& oldPath, const std::string& newPath);

}  // namespace DownloadReview
