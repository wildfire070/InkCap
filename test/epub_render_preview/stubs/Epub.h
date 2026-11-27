#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class Epub {
 public:
  bool optimizerImageAvailable = false;
  uint16_t optimizerImageWidth = 0;
  uint16_t optimizerImageHeight = 0;
  mutable size_t streamReadCount = 0;

  enum class StreamEarlyStop { Disabled, Probe, Immediate };

  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output&, size_t,
                                StreamEarlyStop = StreamEarlyStop::Disabled) const {
    ++streamReadCount;
    return false;
  }
  bool extractItemToFile(const std::string&, const std::string&) const { return false; }
  bool getOptimizerImageDimensions(const std::string&, uint16_t& width, uint16_t& height) const {
    if (!optimizerImageAvailable) return false;
    width = optimizerImageWidth;
    height = optimizerImageHeight;
    return true;
  }
};
