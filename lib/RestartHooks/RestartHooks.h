#pragma once

// Lightweight extension point so a branch-specific feature (e.g. Companion,
// which only exists on some branches) can react before any hard ESP.restart(),
// without coupling shared/library code (GfxRenderer's buffer-loan failure
// paths, AO3SyncActivity, main.cpp's silent-restart path) to that feature
// directly. No-op until something registers a hook; safe to call
// unconditionally from anywhere a restart is about to happen.
using PreRestartHook = void (*)();

void setPreRestartHook(PreRestartHook hook);
void runPreRestartHook();
