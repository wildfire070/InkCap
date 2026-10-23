add_executable(CoverToneCurveTest
  CoverToneCurveTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/CoverToneCurve.cpp
)

target_include_directories(CoverToneCurveTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(CoverToneCurveTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoverToneCurveTest)
