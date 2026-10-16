#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
inline std::map<std::string, std::string> files;
inline std::set<std::string> directories;
inline std::set<std::string> failRemove, failRename;
inline bool failClose = false, failRead = false, failIteration = false;
inline std::map<std::string, int> openReaders;
struct FsFile {
  std::string path;
  size_t position = 0, child = 0;
  bool opened = false, directory = false;
  std::vector<std::string> children;
  FsFile() = default;
  explicit FsFile(const std::string& p)
      : path(p), opened(files.count(p) || directories.count(p)), directory(directories.count(p)) {
    if (directory) {
      for (const auto& kv : files)
        if (kv.first.substr(0, p.size() + 1) == p + "/" && kv.first.find('/', p.size() + 1) == std::string::npos)
          children.push_back(kv.first);
      for (const auto& d : directories)
        if (d.substr(0, p.size() + 1) == p + "/" && d.find('/', p.size() + 1) == std::string::npos)
          children.push_back(d);
    } else if (opened) {
      // Hardware does not support concurrent readers of one path.
      if (openReaders[p])
        opened = false;
      else
        ++openReaders[p];
    }
  }
  FsFile(const FsFile&) = delete;
  FsFile(FsFile&& o) { *this = std::move(o); }
  FsFile& operator=(FsFile&& o) {
    close();
    path = std::move(o.path);
    position = o.position;
    child = o.child;
    opened = std::exchange(o.opened, false);
    directory = o.directory;
    children = std::move(o.children);
    return *this;
  }
  ~FsFile() { close(); }
  operator bool() const { return opened; }
  bool isDirectory() const { return directory; }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return failIteration; }
  FsFile openNextFile() { return !failIteration && child < children.size() ? FsFile(children[child++]) : FsFile(); }
  size_t getName(char* out, size_t size) {
    const auto name = path.substr(path.find_last_of('/') + 1);
    std::snprintf(out, size, "%s", name.c_str());
    return name.size();
  }
  size_t fileSize() const { return files.at(path).size(); }
  uint64_t fileSize64() const { return files.at(path).size(); }
  bool seek(size_t offset) {
    if (!opened || directory || offset > files.at(path).size()) return false;
    position = offset;
    return true;
  }
  int read(void* out, size_t size) {
    if (failRead || !opened || directory) return -1;
    size = std::min(size, files[path].size() - position);
    std::memcpy(out, files[path].data() + position, size);
    position += size;
    return int(size);
  }
  bool close() {
    if (opened && !directory) --openReaders[path];
    opened = false;
    return !failClose;
  }
};
struct StorageFake {
  bool exists(const char* p) { return files.count(p) || directories.count(p); }
  FsFile open(const char* p) { return FsFile(p); }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    f = FsFile(p);
    return bool(f);
  }
  bool ensureDirectoryExists(const char* p) {
    directories.insert(p);
    return true;
  }
  bool remove(const char* p) { return !failRemove.count(p) && files.erase(p); }
  bool rename(const char* a, const char* b) {
    if (failRename.count(a) || !files.count(a) || exists(b)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
};
inline StorageFake Storage;

using HalFile = FsFile;
