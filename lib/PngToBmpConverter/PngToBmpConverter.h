#pragma once

#include <HalStorage.h>

#include "../GfxRenderer/ImageToneProfile.h"

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, bool adaptiveContain = false,
                                         ImageToneProfile toneProfile = ImageToneProfile::Legacy);

 public:
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true,
                                 ImageToneProfile toneProfile = ImageToneProfile::Legacy);
  static bool pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool adaptiveContain = false);
  // Source pixel dimensions only, no pixel decode -- for picking a thumbnail
  // target aspect ratio before generating one.
  static bool peekDimensions(FsFile& pngFile, int* outWidth, int* outHeight);
};
