#pragma once
#include <functional>
#include <string>
#include <vector>

/**
 * Whole-SD-card recursive book enumeration, shared by anything that needs
 * to see every book on the device rather than one directory at a time (the
 * BookFusion bulk metadata refresh).
 *
 * FileBrowserActivity's own directory listing stays folder-by-folder and
 * doesn't need this -- this is specifically for "every book, regardless of
 * where it lives."
 */
namespace LibraryScan {

using BookVisitor = std::function<void(const std::string& path)>;

// Recursively scans from the SD root for .epub/.xtc files, skipping hidden
// (dot-prefixed) directories such as /.crosspoint and /.sleep, calling
// visitor once per book found. Blocking; can take a few seconds on a large
// library -- callers should show a loading indicator around this call.
//
// Streaming rather than returning a std::vector: a caller building its own
// per-book state would otherwise hold two full copies of every path in
// memory at once -- the returned vector here, and whatever it copies each
// path into. Feed the visitor directly into the caller's own structure
// instead.
void enumerateBooks(const BookVisitor& visitor);

}  // namespace LibraryScan
