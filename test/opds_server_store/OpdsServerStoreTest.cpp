#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "util/OpdsDownloadFolder.h"

// The legacy global settings are outside this store's test; migration uses only
// their existing OPDS fields and save method.
void CrossPointSettings::toJson(JsonDocument&) const {}
uint8_t CrossPointSettings::defaultUiScale() { return 1; }
bool CrossPointSettings::saveToFile() const { return true; }

TEST(OpdsServerStoreTest, ExistingValuesAndUnknownDefaultsRemainCompatible) {
  EXPECT_EQ(static_cast<unsigned>(OpdsFilenameFormat::AUTHOR_TITLE), 0u);
  EXPECT_EQ(static_cast<unsigned>(OpdsFilenameFormat::TITLE_AUTHOR), 1u);
  EXPECT_EQ(opdsFilenameFormatFromJson(nullptr), OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(opdsFilenameFormatFromJson(""), OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(opdsFilenameFormatFromJson("future_format"), OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_STREQ(opdsFilenameFormatToJson(static_cast<OpdsFilenameFormat>(255)), "author_title");
}

TEST(OpdsServerStoreTest, SavesAndReloadsAllThreeFormatsWithoutChangingCredentials) {
  Storage.reset();
  auto& store = OPDS_STORE;
  store.release();
  store.loadFromFile();
  const OpdsFilenameFormat formats[] = {OpdsFilenameFormat::AUTHOR_TITLE, OpdsFilenameFormat::TITLE_AUTHOR,
                                        OpdsFilenameFormat::SERVER_FILENAME};
  for (const auto format : formats) {
    OpdsServer server{"Library", "https://example.test/opds", "reader", "secret", format, {}};
    ASSERT_TRUE(store.addServer(server));
  }
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  ASSERT_EQ(store.getCount(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_NE(store.getServer(i), nullptr);
    EXPECT_EQ(store.getServer(i)->filenameFormat, formats[i]);
    EXPECT_EQ(store.getServer(i)->username, "reader");
    EXPECT_EQ(store.getServer(i)->password, "secret");
  }
}

TEST(OpdsServerStoreTest, OldMissingOrUnknownFieldsLoadAsAuthorTitle) {
  Storage.reset();
  Storage.put(
      OpdsServerStore::getFilePath(),
      R"({"servers":[{"name":"old"},{"filenameFormat":"future_format"},{"filenameFormat":"title_author"},{"filenameFormat":"server_filename"}]})");
  auto& store = OPDS_STORE;
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  ASSERT_EQ(store.getCount(), 4u);
  EXPECT_EQ(store.getServer(0)->filenameFormat, OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(store.getServer(1)->filenameFormat, OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(store.getServer(2)->filenameFormat, OpdsFilenameFormat::TITLE_AUTHOR);
  EXPECT_EQ(store.getServer(3)->filenameFormat, OpdsFilenameFormat::SERVER_FILENAME);
}

TEST(OpdsServerStoreTest, DownloadFoldersRoundTripAndOldServersInheritLiveDefault) {
  Storage.reset();
  Storage.put(
      OpdsServerStore::getFilePath(),
      R"({"servers":[{"name":"Old"},{"name":"Root","downloadFolder":"/"},{"name":"Custom","downloadFolder":" Books//Calibre/ "}]})");
  auto& store = OPDS_STORE;
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  EXPECT_TRUE(store.getServer(0)->downloadFolder.empty());
  EXPECT_EQ(store.getServer(1)->downloadFolder, "/");
  EXPECT_EQ(store.getServer(2)->downloadFolder, "/Books/Calibre");
  EXPECT_STREQ(OpdsDownloadFolder::effective(store.getServer(0)->downloadFolder, "/NewDefault"), "/NewDefault");
  EXPECT_STREQ(OpdsDownloadFolder::effective(store.getServer(1)->downloadFolder, "/NewDefault"), "/");
  EXPECT_STREQ(OpdsDownloadFolder::effective(store.getServer(2)->downloadFolder, "/NewDefault"), "/Books/Calibre");
  ASSERT_TRUE(store.saveToFile());
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  EXPECT_TRUE(store.getServer(0)->downloadFolder.empty());
  EXPECT_EQ(store.getServer(1)->downloadFolder, "/");
  EXPECT_EQ(store.getServer(2)->downloadFolder, "/Books/Calibre");
}

TEST(OpdsServerStoreTest, PartialJsonUpdatesPreserveOrExplicitlyClearOverride) {
  JsonDocument doc;
  std::string folder = "/Books/Calibre";
  ASSERT_EQ(deserializeJson(doc, R"({"name":"Older client"})"), DeserializationError::Ok);
  EXPECT_TRUE(applyOpdsDownloadFolder(doc.as<JsonVariantConst>(), folder));
  EXPECT_EQ(folder, "/Books/Calibre");
  doc["downloadFolder"] = "";
  EXPECT_TRUE(applyOpdsDownloadFolder(doc.as<JsonVariantConst>(), folder));
  EXPECT_TRUE(folder.empty());
  doc["downloadFolder"] = "/";
  EXPECT_TRUE(applyOpdsDownloadFolder(doc.as<JsonVariantConst>(), folder));
  EXPECT_EQ(folder, "/");
  for (const char* json : {R"({"downloadFolder":null})", R"({"downloadFolder":42})", R"({"downloadFolder":[]})",
                           R"({"downloadFolder":"/Books/../secret"})", R"({"downloadFolder":"/Books\u0000hidden"})"}) {
    ASSERT_EQ(deserializeJson(doc, json), DeserializationError::Ok);
    EXPECT_FALSE(applyOpdsDownloadFolder(doc.as<JsonVariantConst>(), folder)) << json;
    EXPECT_EQ(folder, "/");
  }
}

TEST(OpdsServerStoreTest, InvalidFoldersAndFailedSavesLeaveServerUnchanged) {
  Storage.reset();
  auto& store = OPDS_STORE;
  store.release();
  store.loadFromFile();
  OpdsServer server;
  server.name = "Calibre";
  server.downloadFolder = "/Books/Calibre";
  Storage.failNextWrite();
  EXPECT_FALSE(store.addServer(server));
  EXPECT_EQ(store.getCount(), 0u);
  ASSERT_TRUE(store.addServer(server));
  server.downloadFolder = "/Books/New";
  Storage.failNextWrite();
  EXPECT_FALSE(store.updateServer(0, server));
  EXPECT_EQ(store.getServer(0)->downloadFolder, "/Books/Calibre");
  server.downloadFolder = "/Books/../secret";
  EXPECT_FALSE(store.updateServer(0, server));
  EXPECT_EQ(store.getServer(0)->downloadFolder, "/Books/Calibre");
  EXPECT_FALSE(store.addServer(server));
  EXPECT_EQ(store.getCount(), 1u);
}

TEST(OpdsDownloadFolderTest, NormalizesPathsAndRejectsUnsafeOrOverlongValues) {
  std::string result = "unchanged";
  for (const auto& input : {"", " \t ", "/", "///", "Books", " /Books//Calibre/ \t"}) {
    ASSERT_TRUE(OpdsDownloadFolder::normalize(input, result)) << input;
    EXPECT_TRUE(result.empty() || result.front() == '/');
  }
  EXPECT_EQ(result, "/Books/Calibre");
  for (const auto& input : {"/Books/../Hidden", "/Books/./Hidden", "/Books/name.", "/Books/name /More", "/Books/a:b",
                            "/Books/a\\b", "/Books/a\nb"}) {
    result = "unchanged";
    EXPECT_FALSE(OpdsDownloadFolder::normalize(input, result)) << input;
    EXPECT_EQ(result, "unchanged");
  }
  EXPECT_TRUE(OpdsDownloadFolder::normalize("/" + std::string(62, 'a'), result));
  EXPECT_FALSE(OpdsDownloadFolder::normalize("/" + std::string(63, 'a'), result));
  EXPECT_FALSE(OpdsDownloadFolder::normalize(std::string(63, 'a'), result));
  // The limit is UTF-8 bytes, not characters; never truncate a multi-byte name.
  std::string utf8;
  for (int i = 0; i < 31; ++i) utf8 += "é";
  EXPECT_TRUE(OpdsDownloadFolder::normalize(utf8, result));
  utf8 += "é";
  EXPECT_FALSE(OpdsDownloadFolder::normalize(utf8, result));
}
