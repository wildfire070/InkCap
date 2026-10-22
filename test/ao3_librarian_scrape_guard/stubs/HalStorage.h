#pragma once

#include <fcntl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Adapted from test/library_builder/stubs/HalStorage.h (an in-memory virtual filesystem proven
// against real production code elsewhere in this repo) with available() added for
// Serialization.h's tryReadString().
namespace fake {

struct Node {
  bool directory = false;
  uint32_t time = 1;
  uint32_t created = 1;
  std::vector<uint8_t> bytes;
};

inline std::map<std::string, std::shared_ptr<Node>> files;

inline void reset() { files.clear(); }

inline void add(const std::string& path, const std::string& bytes = "") {
  auto node = std::make_shared<Node>();
  node->bytes.assign(bytes.begin(), bytes.end());
  files[path] = node;
  std::string parent = path.substr(0, path.find_last_of('/'));
  if (parent.empty()) parent = "/";
  if (!files.count(parent)) {
    add(parent, "");
    files[parent]->directory = true;
  }
}

}  // namespace fake

class HalFile {
 public:
  std::shared_ptr<fake::Node> node;
  std::string path;
  size_t pos = 0;
  bool iterationFailed_ = false;
  std::shared_ptr<std::vector<std::string>> children;

  explicit operator bool() const { return bool(node); }
  bool isOpen() const { return bool(node); }
  bool close() {
    node.reset();
    return true;
  }
  bool isDirectory() const { return node && node->directory; }
  void rewindDirectory() {
    pos = 0;
    children.reset();
  }
  bool allocationFailed() const { return false; }
  bool iterationFailed() const { return iterationFailed_; }
  HalFile openNextFile() {
    if (!children) {
      children = std::make_shared<std::vector<std::string>>();
      for (const auto& [name, value] : fake::files) {
        std::string parent = name.substr(0, name.find_last_of('/'));
        if (parent.empty()) parent = "/";
        if (name != path && parent == path) children->push_back(name);
      }
    }
    if (pos >= children->size()) return {};
    HalFile file;
    file.path = (*children)[pos++];
    file.node = fake::files[file.path];
    return file;
  }
  size_t getName(char* out, const size_t size) {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    if (size > 0) {
      std::strncpy(out, name.c_str(), size);
      out[size - 1] = '\0';
    }
    return name.size();
  }
  uint32_t modificationTime() const { return node ? node->time : 0; }
  uint32_t creationTime() const { return node ? node->created : 0; }
  uint64_t fileSize64() const { return node ? node->bytes.size() : 0; }
  size_t fileSize() const { return static_cast<size_t>(fileSize64()); }
  size_t size() const { return fileSize(); }
  size_t position() const { return pos; }
  int available() const {
    return node ? static_cast<int>(node->bytes.size() - std::min(pos, node->bytes.size())) : 0;
  }
  bool seekSet(const size_t offset) {
    if (!node || (!node->directory && offset > node->bytes.size())) return false;
    pos = offset;
    return true;
  }
  bool seek(const size_t offset) { return seekSet(offset); }
  int read(void* out, size_t size) {
    if (size == 0) return 0;
    if (!node) return -1;
    size = std::min(size, node->bytes.size() - std::min(pos, node->bytes.size()));
    std::memcpy(out, node->bytes.data() + std::min(pos, node->bytes.size()), size);
    pos += size;
    return static_cast<int>(size);
  }
  size_t write(const uint8_t* data, const size_t size) {
    if (size == 0) return 0;
    if (!node) return 0;
    node->bytes.resize(std::max(node->bytes.size(), pos + size));
    std::memcpy(node->bytes.data() + pos, data, size);
    pos += size;
    return size;
  }
  size_t write(const void* data, const size_t size) { return write(static_cast<const uint8_t*>(data), size); }
  bool sync() { return bool(node); }
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool exists(const char* path) const { return fake::files.count(path) != 0; }
  bool mkdir(const char* path) {
    if (!exists(path)) fake::add(path, "");
    fake::files[path]->directory = true;
    return true;
  }
  HalFile open(const char* path) {
    HalFile file;
    const auto found = fake::files.find(path);
    if (found != fake::files.end()) {
      file.node = found->second;
      file.path = path;
    }
    return file;
  }
  HalFile open(const char* path, const int oflag) {
    if ((oflag & O_CREAT) != 0 && !exists(path)) fake::add(path, "");
    HalFile file = open(path);
    if (file && (oflag & O_TRUNC) != 0) file.node->bytes.clear();
    if (file && (oflag & O_APPEND) != 0) file.pos = file.fileSize();
    return file;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = open(path);
    return bool(file);
  }
  bool openFileForRead(const char* module, const std::string& path, HalFile& file) {
    return openFileForRead(module, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    fake::add(path, "");
    file = open(path);
    return true;
  }
  bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
  bool remove(const char* path) { return fake::files.erase(path) != 0; }
  bool removeDir(const char* path) {
    if (!exists(path)) return false;
    const std::string prefix = std::string(path) + "/";
    for (auto it = fake::files.begin(); it != fake::files.end();) {
      if (it->first == path || it->first.starts_with(prefix))
        it = fake::files.erase(it);
      else
        ++it;
    }
    return true;
  }
  bool rename(const char* from, const char* to) {
    if (!exists(from) || exists(to)) return false;
    fake::files[to] = fake::files[from];
    fake::files.erase(from);
    return true;
  }
};

using FsFile = HalFile;

#define Storage HalStorage::getInstance()
