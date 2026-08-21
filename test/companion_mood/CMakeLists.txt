add_executable(CompanionMoodTest
  CompanionMoodTest.cpp
  ${REPO_ROOT}/lib/Companion/CompanionMood.cpp
)

target_include_directories(CompanionMoodTest PRIVATE
  ${REPO_ROOT}/lib/Companion
)

target_link_libraries(CompanionMoodTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionMoodTest)
