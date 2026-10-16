#pragma once

#include <cstddef>
#include <string_view>

#include "AssetCatalog.h"

// SD source management only. Activation remains an explicit installer action.
namespace AssetDownloadService {
constexpr size_t MAX_PATH = 160;
constexpr size_t MAX_URL = 384;
enum class Result { Ok, Io, Memory, Cancelled, Invalid, Collision, OverwriteRequired, Unavailable, Network };
enum class Status { Missing, Current, Previous, Different, FileOnSd, Collision };
struct Source {
  Status status = Status::Missing;
  char path[MAX_PATH] = {};
  char sha256[65] = {};
  // A transaction sidecar remains; explicit Download/Retry performs recovery.
  bool recoveryPending = false;
};
struct Callbacks {
  void* context = nullptr;
  bool (*cancelled)(void*) = nullptr;
  void (*progress)(void*, size_t, size_t) = nullptr;
  void (*verifying)(void*) = nullptr;
  // Companions are verified on SD; the installable payload transfer starts next.
  void (*payloadStarted)(void*) = nullptr;
};
// Firmware-owned approved prefixes, ending in '/'. Empty defaults fail closed.
// No URL or redirect prefix comes from the catalog or user input.
struct Endpoints {
  const char* base = "";
  const char* redirectBase = "";
};
bool allowUrl(std::string_view url, void* endpoints);
bool buildUrl(const Endpoints& endpoints, const char* relative, char* out, size_t capacity);
// Offline entries with an empty sha256 classify existing published sources as FileOnSd.
Result discover(AssetCatalog::Kind kind, const AssetCatalog::Entry& entry, Source& source,
                const Callbacks& callbacks = {});
Result download(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry, const Endpoints& endpoints,
                bool overwriteApproved, const Callbacks& callbacks = {});
const char* resultName(Result result);
}  // namespace AssetDownloadService
