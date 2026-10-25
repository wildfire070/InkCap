#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(std::string imagePath, std::string sourcePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  void prepareCache() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  // Decodes into the on-SD pixel cache ahead of time, without touching the live framebuffer --
  // used by the reader's forward-page-turn prefetch to warm the cache for an upcoming page's
  // image before the user actually turns to it. (x, y) must be where the image will actually be
  // rendered once that page is shown, so the fully-on-screen check (render()'s own caching
  // eligibility) agrees with the real render. Returns false (nothing done, not an error) if the
  // image isn't fully on screen at that position, already has a valid cache, or already failed
  // to decode this session -- all the same no-op conditions render() already has.
  bool prefetchDecode(GfxRenderer& renderer, int x, int y, bool imageGrayscale = true) const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y, bool foregroundBlack) const;
  static void clearSessionRenderFailures();
  static void releaseSessionPixelCache();

  // The section builder only reads image headers. The reader supplies this
  // allocation-free callback pair to seed a bundled pixel cache or extract the
  // original image on its first render.
  using ExtractFn = bool (*)(void* context, const char* sourcePath, const char* destinationPath);
  using SeedCacheFn = bool (*)(void* context, const char* sourcePath, int width, int height,
                               const char* destinationPath);
  static void setExtractor(void* context, ExtractFn extract, SeedCacheFn seedCache);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y, const bool foregroundBlack, bool imageGrayscale = true);
  bool serialize(Print& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  std::string sourcePath;
  int16_t width;
  int16_t height;

  static void* extractContext;
  static ExtractFn extractFn;
  static SeedCacheFn seedCacheFn;
};
