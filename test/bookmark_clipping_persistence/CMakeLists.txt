add_executable(BookmarkStorePersistenceTest
  BookmarkStorePersistenceTest.cpp
  ${REPO_ROOT}/src/BookmarkStore.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  stubs/BookMoveUtilsStub.cpp
)
target_include_directories(BookmarkStorePersistenceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/uzlib/src
)
target_compile_definitions(BookmarkStorePersistenceTest PRIVATE SIMULATOR)
target_link_libraries(BookmarkStorePersistenceTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookmarkStorePersistenceTest)

add_executable(ClippingStorePersistenceTest
  ClippingStorePersistenceTest.cpp
  ${REPO_ROOT}/src/ClippingStore.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
  ${REPO_ROOT}/src/sync/SyncFiles.cpp
  ${REPO_ROOT}/src/sync/SyncPending.cpp
  ${REPO_ROOT}/src/sync/SyncState.cpp
  ${REPO_ROOT}/src/sync/SyncParked.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(ClippingStorePersistenceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/src/sync
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/uzlib/src
)
target_compile_definitions(ClippingStorePersistenceTest PRIVATE SIMULATOR)
target_link_libraries(ClippingStorePersistenceTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ClippingStorePersistenceTest)
