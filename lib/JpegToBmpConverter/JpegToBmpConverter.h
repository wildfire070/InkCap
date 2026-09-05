#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool adaptiveContain = false);

 public:
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              bool adaptiveContain = false);
  // Source pixel dimensions only, no pixel decode -- for picking a thumbnail
  // target aspect ratio before generating one.
  static bool peekDimensions(FsFile& jpegFile, int* outWidth, int* outHeight);
};
