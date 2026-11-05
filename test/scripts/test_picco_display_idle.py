"""Compile the production RenderLock boundary with a recording display/mutex."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/activities/ActivityManager.cpp").read_text()
start = source.index("RenderLock::RenderLock(const Mode mode)")
end = source.index("/**", start)
methods = source[start:end]
harness = r'''
#include <cassert>
#include "RenderLock.h"
bool held = false;
bool available = true;
int shutdowns = 0;
int releases = 0;
// This branch delegates RenderLock to GfxRenderer's own recursive framebuffer
// mutex rather than upstream's plain semaphore -- see the RenderLock constructor's
// own comment in ActivityManager.cpp. Mock that same surface here instead of
// xSemaphoreTake/xSemaphoreGive.
struct {
  struct {
    bool tryLockFrameBufferMutex() {
      if (!available) return false;
      assert(!held);
      held = true;
      return true;
    }
    void lockFrameBufferMutex() {
      assert(available && !held);
      held = true;
    }
    void unlockFrameBufferMutex() { assert(held); held = false; ++releases; }
  } renderer;
} activityManager;
struct {
  void powerOffBetweenFrames() {
    assert(held);  // shutdown must finish BEFORE another drawing task can enter
    ++shutdowns;
  }
} display;
'''
checks = r'''
int main() {
  {
    RenderLock lock;
    assert(held && shutdowns == 0);
    // Intermediate B/W and grayscale work retains the same lock.
    assert(lock.ownsLock() && shutdowns == 0);
  }
  assert(!held && releases == 1 && shutdowns == FREEINK_DEVICE_PICCO);
  {
    RenderLock lock;
    lock.unlock();
    assert(!held && releases == 2 && shutdowns == 2 * FREEINK_DEVICE_PICCO);
    lock.unlock(); // destructor and repeated unlock must not shut down twice
  }
  assert(releases == 2 && shutdowns == 2 * FREEINK_DEVICE_PICCO);
  available = false;
  { RenderLock lock(RenderLock::Mode::Try); assert(!lock.ownsLock()); }
  assert(releases == 2 && shutdowns == 2 * FREEINK_DEVICE_PICCO);
}
'''
with tempfile.TemporaryDirectory(prefix="picco-idle-lock-") as tmp:
    cpp = Path(tmp) / "test.cpp"
    cpp.write_text(harness + methods + checks)
    for picco in (0, 1):
        exe = Path(tmp) / f"test-{picco}"
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                        f"-DFREEINK_DEVICE_PICCO={picco}", "-I" + str(ROOT / "src/activities"),
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("RenderLock idle shutdown: ordering, explicit/destructor release, failed try-lock, non-Picco passed")
