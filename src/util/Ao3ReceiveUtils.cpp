#include "Ao3ReceiveUtils.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "../Ao3Librarian.h"
#include "BookCacheUtils.h"

namespace {
constexpr char PENDING_FILE[] = "/.crosspoint/ao3_received.txt";
constexpr char SETTINGS_FILE[] = "/.crosspoint/ao3_settings.json";
constexpr int MAX_UNIQUE_SUFFIX = 99;

std::string trimSlashes(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
  return path;
}

void writePending(const std::vector<std::string>& paths) {
  std::string body;
  for (const auto& p : paths) {
    body += p;
    body += '\n';
  }
  if (body.empty()) {
    Storage.remove(PENDING_FILE);
    return;
  }
  Storage.writeFile(PENDING_FILE, String(body.c_str()));
}
}  // namespace

namespace Ao3ReceiveUtils {

std::string receiveFolder() {
  if (Storage.exists(SETTINGS_FILE)) {
    String json = Storage.readFile(SETTINGS_FILE);
    if (!json.isEmpty()) {
      JsonDocument doc;
      if (!deserializeJson(doc, json)) {
        const char* configured = doc["receiveFolder"] | "";
        if (configured[0] != '\0') return trimSlashes(configured);
      }
    }
  }
  return DEFAULT_RECEIVE_FOLDER;
}

std::string uniqueFilePath(const std::string& folder, const std::string& fileName) {
  const std::string dir = trimSlashes(folder);
  const std::string prefix = dir == "/" ? "/" : dir + "/";

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

std::vector<std::string> readPending() {
  std::vector<std::string> paths;
  if (!Storage.exists(PENDING_FILE)) return paths;
  const String content = Storage.readFile(PENDING_FILE);
  std::string line;
  for (size_t i = 0; i < content.length(); i++) {
    const char c = content[i];
    if (c == '\n' || c == '\r') {
      if (!line.empty()) paths.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) paths.push_back(line);
  return paths;
}

void appendPending(const std::string& path) {
  std::vector<std::string> paths = readPending();
  if (std::find(paths.begin(), paths.end(), path) != paths.end()) return;
  paths.push_back(path);
  writePending(paths);
}

void removePending(const std::string& path) {
  std::vector<std::string> paths = readPending();
  const auto it = std::remove(paths.begin(), paths.end(), path);
  if (it == paths.end()) return;
  paths.erase(it, paths.end());
  writePending(paths);
}

bool hasPending() { return Storage.exists(PENDING_FILE) && !readPending().empty(); }

bool replaceExisting(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath || !Storage.exists(oldPath.c_str()) || !Storage.exists(newPath.c_str())) return false;

  // Clearing the old book's cache also drops its reading status file, so carry it over.
  const std::string oldCache = Epub::cachePathForFilePath(oldPath, "/.crosspoint");
  const BookStatus status = Ao3Librarian::getBookStatus(oldCache);

  const std::string setAside = oldPath + ".replaced";
  Storage.remove(setAside.c_str());
  if (!Storage.rename(oldPath.c_str(), setAside.c_str())) {
    LOG_ERR("AO3R", "Could not set aside %s", oldPath.c_str());
    return false;
  }
  if (!Storage.rename(newPath.c_str(), oldPath.c_str())) {
    LOG_ERR("AO3R", "Could not move %s into place; restoring the old copy", newPath.c_str());
    Storage.rename(setAside.c_str(), oldPath.c_str());
    return false;
  }
  Storage.remove(setAside.c_str());

  // New file's own cache/index entry no longer describes anything on disk.
  Ao3Librarian::tombstoneRecord(newPath);
  Epub(newPath, "/.crosspoint").clearCache();

  // Derived data for the old path (sections, sidecar, index record) is stale; user state stays.
  clearBookCachePreservingUserState(oldPath);
  Epub restored(oldPath, "/.crosspoint");
  restored.setupCacheDir();
  Ao3Librarian::saveBookStatus(oldCache, status);
  return true;
}

}  // namespace Ao3ReceiveUtils
