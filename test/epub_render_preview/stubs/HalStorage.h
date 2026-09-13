#pragma once

// Real (not stubbed) file I/O backed by the host filesystem -- unlike every
// other test target's no-op HalStorage stub, EpubRenderPreview needs
// ChapterHtmlSlimParser::parseAndBuildPages() to genuinely open and read a
// real chapter .xhtml file from disk, and CssParser::loadFromStream() to
// genuinely read a real stylesheet, exactly as production code does. The
// `moduleName` tag every real call site passes is ignored -- `path` is used
// verbatim as a host filesystem path.

#include <Arduino.h>
#include <Print.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

class HalFile : public Print {
 public:
  HalFile() = default;

  bool openForRead(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    cursor_ = 0;
    open_ = true;
    writing_ = false;
    return true;
  }

  bool openForWrite(const std::string& path) {
    path_ = path;
    bytes_.clear();
    cursor_ = 0;
    open_ = true;
    writing_ = true;
    return true;
  }

  int available() const { return open_ ? static_cast<int>(bytes_.size() - std::min(cursor_, bytes_.size())) : 0; }
  size_t position() const { return cursor_; }
  size_t size() const { return bytes_.size(); }
  size_t fileSize() const { return size(); }

  int read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  int read(void* buf, const size_t length) {
    if (!open_) return 0;
    const size_t readable = std::min(length, bytes_.size() - std::min(cursor_, bytes_.size()));
    if (readable > 0) std::memcpy(buf, bytes_.data() + cursor_, readable);
    cursor_ += readable;
    return static_cast<int>(readable);
  }

  bool seek(const size_t position) {
    if (!open_ || position > bytes_.size()) return false;
    cursor_ = position;
    return true;
  }
  bool seekSet(const size_t position) { return seek(position); }
  bool seekCur(const int offset) { return seek(cursor_ + offset); }

  bool close() {
    if (writing_ && open_) {
      std::ofstream out(path_, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    }
    open_ = false;
    return true;
  }
  bool isOpen() const { return open_; }
  operator bool() const { return open_; }
  bool sync() const { return open_; }

  size_t write(const uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, const size_t length) override {
    if (!open_) return 0;
    if (cursor_ + length > bytes_.size()) bytes_.resize(cursor_ + length);
    if (length > 0) std::memcpy(bytes_.data() + cursor_, data, length);
    cursor_ += length;
    return length;
  }
  size_t write(const void* data, const size_t length) { return write(static_cast<const uint8_t*>(data), length); }

 private:
  std::vector<uint8_t> bytes_;
  size_t cursor_ = 0;
  bool open_ = false;
  bool writing_ = false;
  std::string path_;
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.openForRead(path); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.openForWrite(path); }
  bool remove(const char* path) { return std::remove(path) == 0; }
  bool exists(const char* path) const {
    std::ifstream in(path);
    return static_cast<bool>(in);
  }
  bool rename(const char* oldPath, const char* newPath) { return std::rename(oldPath, newPath) == 0; }
};

#define Storage HalStorage::getInstance()
