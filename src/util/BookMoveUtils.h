#pragma once

#include <string>

namespace BookMoveUtils {

enum class RenameMigrationResult {
  Success,
  RolledBack,
  KeepRenamed,
};

std::string buildReadFolderDestination(const std::string& srcPath);
RenameMigrationResult migrateRenamedBookState(const std::string& oldPath, const std::string& newPath,
                                              const std::string& oldCachePath, const std::string& title,
                                              const std::string& author, const char* bookType);
bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, bool keepInRecents);

}  // namespace BookMoveUtils
