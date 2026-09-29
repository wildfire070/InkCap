add_executable(ReaderSettingsSnapshotIOTest
  ReaderSettingsSnapshotIOTest.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderSettingsSnapshotIO.cpp
)

target_include_directories(ReaderSettingsSnapshotIOTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(ReaderSettingsSnapshotIOTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderSettingsSnapshotIOTest)
