#pragma once
#include <string>

class Epub;

/**
 * Fetches and caches a BookFusion book's cover art on disk.
 *
 * download() and convert() are split so a caller can lend the framebuffer to
 * different subsystems for each phase (network vs. JPEG/PNG decode scratch)
 * without the two ever aliasing memory -- see call sites in
 * BookFusionBrowserActivity.cpp and RefreshBookFusionMetadataActivity.cpp.
 * refresh() is the two combined, for callers that don't need that split.
 *
 * All three write into epub's existing cache paths (Epub::getThumbBmpPath(),
 * Epub::getCoverBmpPath()) using the same JPEG/PNG-to-BMP converters
 * Epub::generateCoverBmp() already uses for embedded covers, so nothing
 * downstream (home screen, sleep screen, reader) needs to know a cover came
 * from BookFusion instead of the EPUB itself.
 */
namespace BookFusionCoverCache {

// download() + convert(). Returns false if coverUrl is empty or either step
// fails; failure is not fatal to whatever triggered it (a missing cover
// just means the caller keeps showing a placeholder).
bool refresh(const std::string& coverUrl, const Epub& epub, int coverHeight);

// Downloads coverUrl to a temp file in epub's cache dir. Network-only, no
// decode. False if coverUrl is empty or the download fails.
bool download(const std::string& coverUrl, const Epub& epub);

// Decodes the temp file downloaded by download() and writes the home-screen
// thumbnail (Epub::getThumbBmpPath(coverHeight)) and the two sleep-screen
// variants (Epub::getCoverBmpPath(false)/(true)). The temp file is removed
// either way. Returns true only if the thumbnail succeeded -- the sleep
// covers are best-effort.
bool convert(const Epub& epub, int coverHeight);

}  // namespace BookFusionCoverCache
