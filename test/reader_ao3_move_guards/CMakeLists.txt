add_executable(ReaderAo3MoveGuardsTest
  ReaderAo3MoveGuardsTest.cpp
)

target_include_directories(ReaderAo3MoveGuardsTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderAo3MoveGuardsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderAo3MoveGuardsTest)
