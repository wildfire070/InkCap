#include "RestartHooks.h"

namespace {
PreRestartHook g_preRestartHook = nullptr;
}

void setPreRestartHook(PreRestartHook hook) { g_preRestartHook = hook; }

void runPreRestartHook() {
  if (g_preRestartHook) g_preRestartHook();
}
