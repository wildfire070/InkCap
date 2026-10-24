#pragma once

#include <Print.h>
#include <fcntl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct HostFileData {
  std::vector<uint8_t> bytes;
  size_t failAt = std::numeric_limits<size_t>::max();
};

class HalFile : public Print {
 public:
  HalFile() = default;
  explicit HalFile(std::shared_ptr<HostFileData> data) : data_(std::move(data)) {}

  void flush() {}
  size_t size() const { return data_ ? data_->bytes.size() : 0; }
  size_t fileSize() const { return size(); }
  int available() const {
    return data_ ? static_cast<int>(data_->bytes.size() - std::min(cursor_, data_->bytes.size())) : 0;
  }
  size_t position() const { return cursor_; }

  int read(void* output, const size_t length) {
    if (!data_) return 0;
    const size_t readable = std::min(length, data_->bytes.size() - std::min(cursor_, data_->bytes.size()));
    if (readable > 0) std::copy_n(data_->bytes.data() + cursor_, readable, static_cast<uint8_t*>(output));
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    if (!data_ || position > data_->bytes.size()) return false;
    cursor_ = position;
    return true;
  }
  bool seekSet(const size_t position) { return seek(position); }
  bool seekCur(const int32_t offset) {
    if (!data_) return false;
    const int64_t next = static_cast<int64_t>(cursor_) + offset;
    if (next < 0 || static_cast<uint64_t>(next) > data_->bytes.size()) return false;
    cursor_ = static_cast<size_t>(next);
    return true;
  }

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* input, const size_t length) override {
    if (!data_ || cursor_ >= data_->failAt) return 0;
    const size_t writable = std::min(length, data_->failAt - cursor_);
    if (cursor_ + writable > data_->bytes.size()) data_->bytes.resize(cursor_ + writable);
    if (writable > 0) std::copy_n(input, writable, data_->bytes.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += writable;
    return writable;
  }
  size_t write(const void* input, const size_t length) { return write(static_cast<const uint8_t*>(input), length); }

  bool sync() const { return static_cast<bool>(data_); }
  bool close() {
    data_.reset();
    cursor_ = 0;
    return true;
  }
  bool isOpen() const { return static_cast<bool>(data_); }
  explicit operator bool() const { return isOpen(); }

  // Directory-iteration surface used only by ClippingStore::nextClippedEpub(), which none of
  // this test suite's scenarios reach (they never populate a directory entry for CLIPPINGS_DIR
  // itself -- Storage.exists(CLIPPINGS_DIR) is false, so the real function returns before any of
  // these would run). Present only so ClippingStore.cpp compiles as a whole translation unit.
  bool isDirectory() const { return false; }
  HalFile openNextFile() { return HalFile{}; }
  size_t getName(char*, size_t) const { return 0; }

 private:
  std::shared_ptr<HostFileData> data_;
  size_t cursor_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void reset() { files_.clear(); }
  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files_.contains(path); }
  bool remove(const char* path) { return files_.erase(path) > 0; }
  bool rename(const char* from, const char* to) {
    const auto found = files_.find(from);
    if (found == files_.end() || files_.contains(to)) return false;
    files_[to] = found->second;
    files_.erase(found);
    return true;
  }

  // Covers both this test suite's simple O_RDONLY reads and ClippingStore's real
  // O_WRONLY|O_CREAT|O_TRUNC temp-file write path.
  HalFile open(const char* path, int flags = O_RDONLY) {
    auto found = files_.find(path);
    if (found == files_.end()) {
      if (!(flags & O_CREAT)) return HalFile{};
      found = files_.emplace(path, std::make_shared<HostFileData>()).first;
    } else if (flags & O_TRUNC) {
      found->second->bytes.clear();
    }
    HalFile file(found->second);
    if (flags & O_APPEND) file.seekSet(found->second->bytes.size());
    return file;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file = open(path.c_str(), O_RDONLY);
    return static_cast<bool>(file);
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    auto data = std::make_shared<HostFileData>();
    files_[path] = data;
    file = HalFile(std::move(data));
    return true;
  }

  // Minimal placeholder -- no production BookmarkStore/ClippingStore path reaches this with a
  // real directory listing in the scenarios this test suite exercises (BookMoveUtils's stub
  // isInArchiveFolder() always reports false, so the real listFiles()-driven archive-recovery
  // scan short-circuits before it would ever be called; ClippingStore's own streaming archive
  // scan, nextClippedEpub(), is likewise never reached by these round-trip scenarios).
  std::vector<std::string> listFiles(const char*, size_t limit = 0) const {
    (void)limit;
    return {};
  }

  void failWritesAt(const std::string& path, size_t offset) { files_.at(path)->failAt = offset; }
  const std::vector<uint8_t>& bytes(const std::string& path) const { return files_.at(path)->bytes; }
  void put(const std::string& path, std::vector<uint8_t> bytes) {
    auto data = std::make_shared<HostFileData>();
    data->bytes = std::move(bytes);
    files_[path] = std::move(data);
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<HostFileData>> files_;
};

#define Storage HalStorage::getInstance()
