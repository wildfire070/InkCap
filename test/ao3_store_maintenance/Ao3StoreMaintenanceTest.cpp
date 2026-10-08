#include "Ao3StoreMaintenance.h"

#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "Ao3MarkedForLaterStore.h"
#include "Ao3NewChaptersStore.h"

namespace {
bool contains(const std::vector<Ao3MarkedForLaterEntry>& entries, const std::string& path) {
  return std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return e.path == path; });
}
bool contains(const std::vector<Ao3NewChaptersEntry>& entries, const std::string& path) {
  return std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return e.path == path; });
}
}  // namespace

// The two AO3 queues are process-lifetime singletons (PersistableStore<T>'s
// getInstance()) with no test-reset hook, so every test uses its own unique
// synthetic paths and only asserts on those specific entries rather than on
// total counts -- state from other tests in the same binary can coexist.
class Ao3StoreMaintenanceTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(Ao3StoreMaintenanceTest, RemovesMarkedForLaterEntryWhoseFileIsGone) {
  constexpr char kGonePath[] = "/gone-marked-1.epub";
  AO3_MARKED_FOR_LATER_STORE.addBook(kGonePath, "Title", "Author");
  ASSERT_TRUE(contains(AO3_MARKED_FOR_LATER_STORE.getEntries(), kGonePath));

  selfHealAo3PathStores();

  EXPECT_FALSE(contains(AO3_MARKED_FOR_LATER_STORE.getEntries(), kGonePath));
}

TEST_F(Ao3StoreMaintenanceTest, KeepsMarkedForLaterEntryWhoseFileStillExists) {
  constexpr char kPresentPath[] = "/present-marked-1.epub";
  Storage.put(kPresentPath, "");
  AO3_MARKED_FOR_LATER_STORE.addBook(kPresentPath, "Title", "Author");
  ASSERT_TRUE(contains(AO3_MARKED_FOR_LATER_STORE.getEntries(), kPresentPath));

  selfHealAo3PathStores();

  EXPECT_TRUE(contains(AO3_MARKED_FOR_LATER_STORE.getEntries(), kPresentPath));
}

TEST_F(Ao3StoreMaintenanceTest, RemovesNewChaptersEntryWhoseFileIsGone) {
  constexpr char kGonePath[] = "/gone-newchap-1.epub";
  AO3_NEW_CHAPTERS_STORE.addBook(kGonePath, "Title", "Author");
  ASSERT_TRUE(contains(AO3_NEW_CHAPTERS_STORE.getEntries(), kGonePath));

  selfHealAo3PathStores();

  EXPECT_FALSE(contains(AO3_NEW_CHAPTERS_STORE.getEntries(), kGonePath));
}

TEST_F(Ao3StoreMaintenanceTest, KeepsNewChaptersEntryWhoseFileStillExists) {
  constexpr char kPresentPath[] = "/present-newchap-1.epub";
  Storage.put(kPresentPath, "");
  AO3_NEW_CHAPTERS_STORE.addBook(kPresentPath, "Title", "Author");
  ASSERT_TRUE(contains(AO3_NEW_CHAPTERS_STORE.getEntries(), kPresentPath));

  selfHealAo3PathStores();

  EXPECT_TRUE(contains(AO3_NEW_CHAPTERS_STORE.getEntries(), kPresentPath));
}
