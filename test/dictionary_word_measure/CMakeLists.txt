add_executable(DictionaryWordMeasureTest
  DictionaryWordMeasureTest.cpp
  ${REPO_ROOT}/src/activities/reader/DictionaryWordMeasure.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DictionaryWordMeasureTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(DictionaryWordMeasureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryWordMeasureTest)
