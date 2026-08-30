#include "BookFusionCoverCache.h"

#include <Epub.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include "HttpDownloader.h"

namespace BookFusionCoverCache {

namespace {

std::string tempCoverPath(const Epub& epub) { return epub.getCachePath() + "/.bookfusion-cover"; }

std::string normalizeUrl(const std::string& coverUrl) {
  if (coverUrl.empty()) return coverUrl;
  if (coverUrl.rfind("//", 0) == 0) return "https:" + coverUrl;
  if (coverUrl[0] == '/') return "https://www.bookfusion.com" + coverUrl;
  return coverUrl;
}

enum class ImageFormat { UNKNOWN, JPEG, PNG };

ImageFormat sniffFormat(HalFile& file) {
  uint8_t magic[8] = {};
  const int n = file.read(magic, sizeof(magic));
  file.seek(0);
  if (n < 3) return ImageFormat::UNKNOWN;
  if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return ImageFormat::JPEG;
  if (n >= 8 && magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47 && magic[4] == 0x0D &&
      magic[5] == 0x0A && magic[6] == 0x1A && magic[7] == 0x0A) {
    return ImageFormat::PNG;
  }
  return ImageFormat::UNKNOWN;
}

// Decodes the already-downloaded temp file into one output BMP, using
// whichever converter matches the sniffed format. Removes destPath on
// failure so a half-written BMP is never mistaken for a cached cover.
bool convertOne(HalFile& srcFile, ImageFormat format, const std::string& destPath, int targetWidth,
                int targetHeight, bool oneBit, bool crop) {
  srcFile.seek(0);
  HalFile destFile;
  if (!Storage.openFileForWrite("BFCC", destPath, destFile)) {
    LOG_ERR("BFCC", "convert: failed to open %s for write", destPath.c_str());
    return false;
  }

  bool success = false;
  if (targetWidth > 0 || targetHeight > 0) {
    success = oneBit ? (format == ImageFormat::JPEG
                            ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(srcFile, destFile, targetWidth,
                                                                                  targetHeight)
                            : PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(srcFile, destFile, targetWidth,
                                                                                targetHeight))
                     : (format == ImageFormat::JPEG
                            ? JpegToBmpConverter::jpegFileToBmpStreamWithSize(srcFile, destFile, targetWidth,
                                                                              targetHeight)
                            : PngToBmpConverter::pngFileToBmpStreamWithSize(srcFile, destFile, targetWidth,
                                                                            targetHeight));
  } else {
    success = format == ImageFormat::JPEG ? JpegToBmpConverter::jpegFileToBmpStream(srcFile, destFile, crop)
                                          : PngToBmpConverter::pngFileToBmpStream(srcFile, destFile, crop);
  }
  destFile.close();

  if (!success) {
    LOG_ERR("BFCC", "convert: decode failed for %s", destPath.c_str());
    Storage.remove(destPath.c_str());
  }
  return success;
}

}  // namespace

bool download(const std::string& coverUrl, const Epub& epub) {
  const std::string url = normalizeUrl(coverUrl);
  if (url.empty()) return false;

  LOG_INF("BFCC", "download: %s -> %s", url.c_str(), tempCoverPath(epub).c_str());
  const auto result = HttpDownloader::downloadToFile(url, tempCoverPath(epub));
  if (result != HttpDownloader::OK) {
    LOG_ERR("BFCC", "download failed (%d): %s", static_cast<int>(result), url.c_str());
    return false;
  }
  LOG_INF("BFCC", "download ok");
  return true;
}

bool convert(const Epub& epub, int coverHeight) {
  const std::string srcPath = tempCoverPath(epub);
  const bool srcExists = Storage.exists(srcPath.c_str());
  if (!srcExists) {
    LOG_ERR("BFCC", "convert: no temp file at %s", srcPath.c_str());
    return false;
  }

  HalFile srcFile;
  bool thumbOk = false;
  if (Storage.openFileForRead("BFCC", srcPath, srcFile)) {
    const ImageFormat format = sniffFormat(srcFile);
    LOG_INF("BFCC", "convert: format=%d thumbPath=%s", static_cast<int>(format), epub.getThumbBmpPath(coverHeight).c_str());
    if (format != ImageFormat::UNKNOWN) {
      // Home-screen thumbnail: 1-bit, matching how Epub::generateThumbBmp()
      // already renders every other book's cover for fast home-screen paint.
      //
      // targetWidth must be an explicit, non-zero value here: passing 0 to
      // the converter doesn't mean "auto-width, scale to coverHeight" the
      // way Epub::getThumbBmpPath()'s own width inference works -- it means
      // "don't scale at all", producing a native-resolution BMP (confirmed
      // on hardware: a 540x776 source cover produced a 540x724 "thumb",
      // rendered oversized wherever something drew it at its own reported
      // size instead of into a fixed box). Compute the same ~2:3 width
      // Epub::getThumbBmpPath(coverHeight) already assumes for this path's
      // filename, so the stored bitmap actually matches what its name and
      // callers expect.
      const int thumbWidth = static_cast<int>((static_cast<int64_t>(coverHeight) * 2 + 1) / 3);
      thumbOk = convertOne(srcFile, format, epub.getThumbBmpPath(coverHeight), thumbWidth, coverHeight, true, true);

      // Sleep-screen covers: fit and cropped variants, same as
      // Epub::generateCoverBmp() produces for an embedded cover. Best-effort
      // -- a missing sleep cover isn't worth failing refresh() over.
      convertOne(srcFile, format, epub.getCoverBmpPath(false), 0, 0, false, false);
      convertOne(srcFile, format, epub.getCoverBmpPath(true), 0, 0, false, true);
    } else {
      LOG_ERR("BFCC", "convert: unrecognized image format");
    }
    srcFile.close();
  } else {
    LOG_ERR("BFCC", "convert: failed to open %s for read", srcPath.c_str());
  }

  Storage.remove(srcPath.c_str());
  LOG_INF("BFCC", "convert: thumbOk=%d", thumbOk);
  return thumbOk;
}

bool refresh(const std::string& coverUrl, const Epub& epub, int coverHeight) {
  if (!download(coverUrl, epub)) return false;
  return convert(epub, coverHeight);
}

}  // namespace BookFusionCoverCache
