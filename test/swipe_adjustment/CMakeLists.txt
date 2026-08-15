add_executable(SwipeAdjustmentTest
  SwipeAdjustmentTest.cpp
)

target_include_directories(SwipeAdjustmentTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
)

target_link_libraries(SwipeAdjustmentTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SwipeAdjustmentTest)
