add_executable(XtcParserTest
  XtcParserTest.cpp
  ${REPO_ROOT}/lib/Xtc/Xtc/XtcParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(XtcParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Xtc/Xtc
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(XtcParserTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(XtcParserTest)
