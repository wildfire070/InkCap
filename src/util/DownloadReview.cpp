#include "DownloadReview.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "Ao3Librarian.h"
#include "Ao3ReceiveUtils.h"

namespace {
constexpr char PENDING_FILE[] = "/.crosspoint/bf_downloads.txt";
constexpr int MAX_UNIQUE_SUFFIX = 99;

void writePending(const std::vector<DownloadReview::Entry>& entries) {
  std::string body;
  for (const auto& entry : entries) {
    body += entry.path;
    if (!entry.collisionOrigin.empty()) {
      body += '\t';
      body += entry.collisionOrigin;
    }
    body += '\n';
  }
  if (body.empty()) {
    Storage.remove(PENDING_FILE);
    return;
  }
  Storage.writeFile(PENDING_FILE, String(body.c_str()));
}
}  // namespace

namespace DownloadReview {

std::string uniqueFilePath(const std::string& folder, const std::string& fileName) {
  std::string dir = folder;
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  const std::string prefix = dir.empty() || dir == "/" ? "" : dir + "/";

  std::string candidate = prefix + fileName;
  if (!Storage.exists(candidate.c_str())) return candidate;

  const size_t dot = fileName.rfind('.');
  const std::string base = dot == std::string::npos ? fileName : fileName.substr(0, dot);
  const std::string ext = dot == std::string::npos ? "" : fileName.substr(dot);
  for (int n = 2; n <= MAX_UNIQUE_SUFFIX; n++) {
    candidate = prefix + base + " (" + std::to_string(n) + ")" + ext;
    if (!Storage.exists(candidate.c_str())) return candidate;
  }
  return "";
}

std::vector<Entry> readPending() {
  std::vector<Entry> entries;
  if (!Storage.exists(PENDING_FILE)) return entries;
  const String content = Storage.readFile(PENDING_FILE);
  std::string line;
  auto flush = [&] {
    if (line.empty()) return;
    Entry entry;
    const size_t tab = line.find('\t');
    entry.path = line.substr(0, tab);
    if (tab != std::string::npos) entry.collisionOrigin = line.substr(tab + 1);
    if (!entry.path.empty()) entries.push_back(entry);
    line.clear();
  };
  for (size_t i = 0; i < content.length(); i++) {
    const char c = content[i];
    if (c == '\n' || c == '\r') {
      flush();
    } else {
      line += c;
    }
  }
  flush();
  return entries;
}

void appendPending(const std::string& path, const std::string& collisionOrigin) {
  std::vector<Entry> entries = readPending();
  for (const auto& entry : entries) {
    if (entry.path == path) return;
  }
  entries.push_back({path, collisionOrigin});
  writePending(entries);
}

void removePending(const std::string& path) {
  std::vector<Entry> entries = readPending();
  const auto it = std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.path == path; });
  if (it == entries.end()) return;
  entries.erase(it, entries.end());
  writePending(entries);
}

bool hasPending() { return Storage.exists(PENDING_FILE) && !readPending().empty(); }

bool findDuplicate(const Entry& entry, const BookIds::Ids& ids, std::string& oldPath) {
  oldPath.clear();
  if (BookIds::findOtherCopy(entry.path, ids, oldPath)) return true;
  // Indexed AO3 fics are found by their cached work ID even if they predate book-ids.json.
  if (!ids.ao3WorkId.empty() && Ao3Librarian::findLivePathByWorkId(ids.ao3WorkId, entry.path, oldPath)) return true;
  if (!entry.collisionOrigin.empty() && entry.collisionOrigin != entry.path &&
      Storage.exists(entry.collisionOrigin.c_str())) {
    oldPath = entry.collisionOrigin;
    return true;
  }
  oldPath.clear();
  return false;
}

bool replaceExisting(const std::string& oldPath, const std::string& newPath) {
  return Ao3ReceiveUtils::replaceExisting(oldPath, newPath);
}

}  // namespace DownloadReview
