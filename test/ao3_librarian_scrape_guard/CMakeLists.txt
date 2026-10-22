add_executable(Ao3LibrarianScrapeGuardTest
  Ao3LibrarianScrapeGuardTest.cpp
  DependencyStubs.cpp
  ${REPO_ROOT}/src/Ao3Librarian.cpp
)

target_include_directories(Ao3LibrarianScrapeGuardTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(Ao3LibrarianScrapeGuardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Ao3LibrarianScrapeGuardTest)
