#include "KOReaderAutoSync.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "SdCardFontSystem.h"
#include "SilentAutoSyncWifi.h"

namespace KOReaderAutoSync {

namespace {
bool armed = false;
int lastSyncedSpine = -1;  // Spine index at the last threshold crossing (or push), for dedup.
float lastSyncedPercent = -1.0f;

// Same tolerance BookFusion's ProgressAutoSync/manual-sync "already synced" checks use.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

std::string documentHashFor(const std::string& epubPath) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(epubPath)
             : KOReaderDocumentId::calculate(epubPath);
}

void performPush(GfxRenderer& renderer, const std::shared_ptr<Epub>& epub, int spineIndex, int pageNumber,
                 int pageCount) {
  const std::string epubPath = epub->getPath();
  const std::string documentHash = documentHashFor(epubPath);
  if (documentHash.empty()) {
    LOG_ERR("KOAuto", "Skipping auto-sync: couldn't compute document hash for %s", epubPath.c_str());
    return;
  }

  // Compute the KOReader xpath position while epub is still loaded -- this streams the current
  // spine item's XHTML, so it has to happen before any network work (and before the caller
  // releases epub for the TLS handshake's heap).
  CrossPointPosition localPos{spineIndex, pageNumber, std::max(1, pageCount)};
  const PositionCoordinateSpace coordinateSpace = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                                                       ? PositionCoordinateSpace::SourceDocument
                                                       : PositionCoordinateSpace::CurrentDocument;
  const KOReaderPosition localProgress = ProgressMapper::toKOReader(epub, localPos, coordinateSpace);
  if (!localProgress.valid) {
    LOG_ERR("KOAuto", "Skipping auto-sync: couldn't map current position to an xpath");
    return;
  }

  bool broughtUpWifi = false;
  if (!SilentAutoSyncWifi::connect(broughtUpWifi)) return;

  LOG_INF("KOAuto", "Starting silent KOReader progress push for %s", documentHash.c_str());
  sdFontSystem.releaseForNetwork(renderer);
  {
    // This branch doesn't have GfxRenderer::NetworkBufferLoan (its HalDisplay layer never grew
    // the release/realloc-framebuffers-to-heap mechanism that requires), so unlike BookFusion's
    // ProgressAutoSync and InkCap/InkCapO3/Capy's KOReaderAutoSync, the framebuffer stays resident
    // through this push -- it gets less heap headroom for the TLS/HTTP work, not none.

    // Check the remote position before pushing: this is push-only and silent, so if the remote
    // is already further ahead (progress made on another device), pushing local would silently
    // regress it. NOT_FOUND (never synced before) and a failed fetch both fall through to a
    // normal push -- fetch failure isn't treated as "remote is ahead".
    KOReaderProgress remote;
    const auto fetchResult = KOReaderSyncClient::getProgress(documentHash, remote);
    if (fetchResult == KOReaderSyncClient::OK && remote.percentage > localProgress.percentage + SAME_PROGRESS_EPSILON) {
      LOG_INF("KOAuto", "Skipping auto-sync push: remote is further ahead (%.1f%% vs local %.1f%%)",
              remote.percentage * 100.0f, localProgress.percentage * 100.0f);
    } else {
      KOReaderProgress progress;
      progress.document = documentHash;
      progress.progress = localProgress.xpath;
      progress.percentage = localProgress.percentage;
      progress.device = SETTINGS.getEffectiveDeviceName();

      if (KOREADER_STORE.usesCrossPointSyncServer()) {
        KOReaderRichPosition pos;
        const float pct =
            localProgress.percentage < 0.0f ? 0.0f : localProgress.percentage > 1.0f ? 1.0f : localProgress.percentage;
        pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
        pos.spineIndex = static_cast<uint16_t>(spineIndex);
        pos.pageNumber = static_cast<uint16_t>(pageNumber);
        pos.totalPages = static_cast<uint16_t>(pageCount > 0 ? pageCount : 1);
        pos.xpath = localProgress.xpath;
        progress.position = std::move(pos);
      }

      if (KOREADER_STORE.getSendMetadata()) {
        KOReaderMetadata meta;
        const auto lastSlash = epubPath.rfind('/');
        meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
        meta.title = epub->getTitle();
        meta.authors = epub->getAuthor();
        progress.metadata = std::move(meta);
      }

      const auto pushResult = KOReaderSyncClient::updateProgress(progress);
      if (pushResult == KOReaderSyncClient::OK) {
        LOG_INF("KOAuto", "Pushed %.1f%% for %s", localProgress.percentage * 100.0f, documentHash.c_str());
      } else {
        LOG_ERR("KOAuto", "Auto-sync push failed for %s: %s", documentHash.c_str(),
                KOReaderSyncClient::errorString(pushResult).c_str());
      }
    }
  }

  // See ProgressAutoSync::performPush()'s matching comment: releaseForNetwork() above freed the
  // reader's resident SD font entirely, and this push fires silently while the reader stays open
  // and keeps rendering immediately after, so it needs to be restored explicitly here.
  sdFontSystem.ensureLoaded(renderer);

  if (broughtUpWifi) SilentAutoSyncWifi::teardown();
}
}  // namespace

void resetSessionBaseline() {
  armed = false;
  lastSyncedSpine = -1;
  lastSyncedPercent = -1.0f;
}

void armIfThresholdCrossed(int spineIndex, float bookPercent) {
  const auto mode = SETTINGS.koreaderAutosyncMode;
  if (mode == CrossPointSettings::AUTOSYNC_OFF || mode == CrossPointSettings::AUTOSYNC_ON_EXIT) return;

  if (mode == CrossPointSettings::AUTOSYNC_EVERY_CHAPTER) {
    if (lastSyncedSpine >= 0 && spineIndex <= lastSyncedSpine) return;
  } else {
    const uint8_t step = SETTINGS.getKoreaderAutosyncPercentStep();
    if (step == 0) return;
    if (lastSyncedPercent >= 0.0f && bookPercent * 100.0f < lastSyncedPercent + static_cast<float>(step)) return;
  }
  armed = true;
}

bool isArmed() { return armed; }

void runIfArmed(GfxRenderer& renderer, const std::shared_ptr<Epub>& epub, int spineIndex, int pageNumber,
                int pageCount, float bookPercent) {
  if (!armed) return;
  armed = false;  // Consumed regardless of outcome -- a failure retries on the next threshold crossing, not a spin.
  lastSyncedSpine = spineIndex;
  lastSyncedPercent = bookPercent * 100.0f;
  if (!epub || !KOREADER_STORE.hasCredentials()) return;

  performPush(renderer, epub, spineIndex, pageNumber, pageCount);
}

void runOnExit(GfxRenderer& renderer, const std::shared_ptr<Epub>& epub, int spineIndex, int pageNumber,
              int pageCount) {
  armed = false;
  if (SETTINGS.koreaderAutosyncMode != CrossPointSettings::AUTOSYNC_ON_EXIT) return;
  if (!epub || !KOREADER_STORE.hasCredentials()) {
    LOG_INF("KOAuto", "Skipping on-exit KOReader auto-sync: not signed in to KOReader sync");
    return;
  }

  performPush(renderer, epub, spineIndex, pageNumber, pageCount);
}

}  // namespace KOReaderAutoSync
