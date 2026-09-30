#pragma once

#include <string>
#include <unordered_map>

#include "WString.h"

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void reset() { files_.clear(); }

  bool exists(const char* path) const { return files_.contains(path); }
  bool mkdir(const char*, bool = true) { return true; }
  bool remove(const char* path) { return files_.erase(path) > 0; }
  bool removeDir(const char* path) { return files_.erase(path) > 0; }
  bool rename(const char* from, const char* to) {
    const auto found = files_.find(from);
    if (found == files_.end() || files_.contains(to)) return false;
    files_[to] = found->second;
    files_.erase(found);
    return true;
  }

  String readFile(const char* path) const {
    const auto found = files_.find(path);
    return found == files_.end() ? String("") : String(found->second);
  }
  bool writeFile(const char* path, const String& content) {
    files_[path] = content.c_str();
    return true;
  }

  // Test helper: seed a file's contents directly.
  void put(const std::string& path, const std::string& content) { files_[path] = content; }

 private:
  std::unordered_map<std::string, std::string> files_;
};

#define Storage HalStorage::getInstance()
