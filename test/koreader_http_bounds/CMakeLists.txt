add_executable(KOReaderHttpBoundsTest
  KOReaderHttpBoundsTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderResponseCap.cpp
)

target_include_directories(KOReaderHttpBoundsTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h for host tests
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderHttpBoundsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderHttpBoundsTest)
